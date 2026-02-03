/* src/race.c
 * Race mode workload generation.
 */
#include "../include/gatebench_race.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"
#include "../include/gatebench_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RACE_SEED_BASE 0x5a17c3d1u
#define RACE_MIN_PKT 64u
#define RACE_MAX_PKT 1500u
#define RACE_INVALID_INTERVAL_NS 1000000u
#define RACE_ERRNO_MAX 4096u
#define RACE_EXTACK_MSG_MAX 128u
#define RACE_EXTACK_SLOTS 6u
#define RACE_INVALID_CASES 7u
#define RACE_THREAD_COUNT 5u

#ifndef NLM_F_ACK_TLVS
#define NLM_F_ACK_TLVS 0x200
#endif

#ifndef NLMSGERR_ATTR_MSG
#define NLMSGERR_ATTR_MSG 1
#endif

struct gb_race_extack_entry {
    char msg[RACE_EXTACK_MSG_MAX];
    uint64_t count;
};

struct gb_race_extack_stats {
    struct gb_race_extack_entry entries[RACE_EXTACK_SLOTS];
    uint64_t other;
};

struct gb_race_nl_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    uint32_t seed;
    uint32_t index;
    uint32_t max_entries;
    uint32_t interval_max;
    int timeout_ms;
    int cpu;
    uint64_t ops;
    uint64_t errors;
    uint64_t err_counts[RACE_ERRNO_MAX];
    struct gb_race_extack_stats extack;
};

struct gb_race_dump_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    uint32_t index;
    int timeout_ms;
    int cpu;
    uint64_t ops;
    uint64_t errors;
    uint64_t err_counts[RACE_ERRNO_MAX];
    struct gb_race_extack_stats extack;
};

struct gb_race_traffic_ctx {
    atomic_bool* stop;
    uint32_t seed;
    int cpu;
    uint64_t ops;
    uint64_t errors;
    uint64_t err_counts[RACE_ERRNO_MAX];
};

struct gb_race_invalid_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    uint32_t seed;
    uint32_t index;
    int timeout_ms;
    int cpu;
    uint64_t ops;
    uint64_t errors;
    uint64_t err_counts[RACE_ERRNO_MAX];
    struct gb_race_extack_stats extack;
};

static uint32_t rng_next(uint32_t* state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static uint32_t rng_range(uint32_t* state, uint32_t max) {
    if (max == 0)
        return 0;
    return (uint32_t)(((uint64_t)rng_next(state) * (uint64_t)max) >> 32);
}

static void race_record_err(uint64_t* errors, uint64_t* err_counts, int ret) {
    uint32_t err;

    if (!errors || !err_counts)
        return;

    if (ret == 0)
        return;

    err = (uint32_t)(ret < 0 ? -ret : ret);
    if (err < RACE_ERRNO_MAX)
        err_counts[err]++;
    (*errors)++;
}

static bool race_parse_extack_msg(const struct gb_nl_msg* resp, char* out, size_t out_len) {
    const struct nlmsghdr* nlh;
    const struct nlmsgerr* err;
    const struct nlattr* attr;
    size_t payload_len;
    size_t err_len;
    int attr_len;

    if (!resp || !resp->buf || !out || out_len == 0)
        return false;

    nlh = (const struct nlmsghdr*)resp->buf;
    if (nlh->nlmsg_type != NLMSG_ERROR)
        return false;

    if (nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(struct nlmsgerr))
        return false;

    payload_len = nlh->nlmsg_len - NLMSG_HDRLEN;
    err_len = NLMSG_ALIGN(sizeof(struct nlmsgerr));
    if (payload_len <= err_len)
        return false;

    err = (const struct nlmsgerr*)((const char*)nlh + NLMSG_HDRLEN);
    if (payload_len - err_len > INT_MAX)
        return false;
    attr_len = (int)(payload_len - err_len);
    attr = (const struct nlattr*)((const char*)err + err_len);

    while (mnl_attr_ok(attr, attr_len)) {
        if (mnl_attr_get_type(attr) == NLMSGERR_ATTR_MSG) {
            const char* msg = mnl_attr_get_str(attr);
            if (msg && msg[0] != '\0') {
                strncpy(out, msg, out_len - 1);
                out[out_len - 1] = '\0';
                return true;
            }
        }
        attr_len -= MNL_ALIGN(attr->nla_len);
        attr = mnl_attr_next(attr);
    }

    return false;
}

static void race_record_extack(struct gb_race_extack_stats* stats, const char* msg) {
    size_t empty = RACE_EXTACK_SLOTS;

    if (!stats || !msg || msg[0] == '\0')
        return;

    for (size_t i = 0; i < RACE_EXTACK_SLOTS; i++) {
        if (stats->entries[i].count == 0 && empty == RACE_EXTACK_SLOTS)
            empty = i;
        if (stats->entries[i].count > 0 && strcmp(stats->entries[i].msg, msg) == 0) {
            stats->entries[i].count++;
            return;
        }
    }

    if (empty < RACE_EXTACK_SLOTS) {
        strncpy(stats->entries[empty].msg, msg, sizeof(stats->entries[empty].msg) - 1);
        stats->entries[empty].msg[sizeof(stats->entries[empty].msg) - 1] = '\0';
        stats->entries[empty].count = 1;
        return;
    }

    stats->other++;
}

static void race_record_nl_error(uint64_t* errors,
                                 uint64_t* err_counts,
                                 struct gb_race_extack_stats* extack,
                                 int ret,
                                 const struct gb_nl_msg* resp) {
    char msg[RACE_EXTACK_MSG_MAX];

    race_record_err(errors, err_counts, ret);
    if (ret >= 0 || !extack || !resp)
        return;

    if (race_parse_extack_msg(resp, msg, sizeof(msg)))
        race_record_extack(extack, msg);
}

static void race_print_err_breakdown(const char* label, uint64_t total, const uint64_t* err_counts) {
    struct race_err_top {
        uint32_t err;
        uint64_t count;
    };
    struct race_err_top top[5];
    size_t topn = sizeof(top) / sizeof(top[0]);

    if (!label || !err_counts)
        return;

    if (total == 0) {
        printf("  %s error breakdown: none\n", label);
        return;
    }

    for (size_t i = 0; i < topn; i++) {
        top[i].err = 0;
        top[i].count = 0;
    }

    for (uint32_t err = 1; err < RACE_ERRNO_MAX; err++) {
        uint64_t count = err_counts[err];
        size_t min_idx = 0;

        if (count == 0)
            continue;

        for (size_t i = 1; i < topn; i++) {
            if (top[i].count < top[min_idx].count)
                min_idx = i;
        }

        if (count > top[min_idx].count) {
            top[min_idx].err = err;
            top[min_idx].count = count;
        }
    }

    for (size_t i = 0; i < topn; i++) {
        for (size_t j = i + 1; j < topn; j++) {
            if (top[j].count > top[i].count) {
                struct race_err_top tmp = top[i];
                top[i] = top[j];
                top[j] = tmp;
            }
        }
    }

    printf("  %s error breakdown:\n", label);
    for (size_t i = 0; i < topn; i++) {
        if (top[i].count == 0)
            break;
        printf("    %s (%u): %llu\n", strerror((int)top[i].err), top[i].err, (unsigned long long)top[i].count);
    }
}

static void race_print_extack(const char* label, const struct gb_race_extack_stats* stats) {
    struct gb_race_extack_entry sorted[RACE_EXTACK_SLOTS];
    size_t used = 0;

    if (!label || !stats)
        return;

    for (size_t i = 0; i < RACE_EXTACK_SLOTS; i++) {
        if (stats->entries[i].count == 0)
            continue;
        sorted[used++] = stats->entries[i];
    }

    for (size_t i = 0; i < used; i++) {
        for (size_t j = i + 1; j < used; j++) {
            if (sorted[j].count > sorted[i].count) {
                struct gb_race_extack_entry tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    if (used == 0 && stats->other == 0) {
        printf("  %s extack breakdown: none\n", label);
        return;
    }

    printf("  %s extack breakdown:\n", label);
    for (size_t i = 0; i < used; i++) {
        printf("    %s: %llu\n", sorted[i].msg, (unsigned long long)sorted[i].count);
    }
    if (stats->other > 0)
        printf("    (other): %llu\n", (unsigned long long)stats->other);
}

static int race_collect_cpus(int* cpus, int max) {
    cpu_set_t set;
    long nproc;
    int count = 0;

    if (!cpus || max <= 0)
        return 0;

    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE && count < max; cpu++) {
            if (CPU_ISSET(cpu, &set))
                cpus[count++] = cpu;
        }
        if (count > 0)
            return count;
    }

    nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0)
        return 0;

    if (nproc > max)
        nproc = max;

    for (int i = 0; i < nproc; i++)
        cpus[i] = i;

    return (int)nproc;
}

static void race_pin_thread(const char* label, int cpu) {
    cpu_set_t set;
    int ret;

    if (cpu < 0)
        return;

    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);

    ret = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (ret != 0) {
        fprintf(stderr, "Race: failed to pin %s thread to CPU %d: %s\n", label ? label : "unknown", cpu, strerror(ret));
    }
}

static int32_t race_random_maxoctets(uint32_t* seed) {
    if (rng_range(seed, 4u) == 0u)
        return -1;
    return (int32_t)(RACE_MIN_PKT + rng_range(seed, 65535u - RACE_MIN_PKT));
}

static void race_shape_init(struct gate_shape* shape, const struct gb_config* cfg) {
    memset(shape, 0, sizeof(*shape));
    shape->clockid = cfg->clockid;
    shape->base_time = cfg->base_time;
    shape->cycle_time = cfg->cycle_time;
    shape->cycle_time_ext = cfg->cycle_time_ext;
    shape->interval_ns = cfg->interval_ns;
    shape->entries = cfg->entries > GB_MAX_ENTRIES ? GB_MAX_ENTRIES : cfg->entries;
}

static struct nlmsghdr* race_gate_nlmsg_start(struct gb_nl_msg* msg,
                                              uint16_t flags,
                                              uint32_t index,
                                              struct nlattr** nest_tab,
                                              struct nlattr** nest_prio,
                                              struct nlattr** nest_opts) {
    struct nlmsghdr* nlh = mnl_nlmsg_put_header(msg->buf);
    struct tcamsg* tca;

    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    *nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    *nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    mnl_attr_put_str(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, index);
    *nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    return nlh;
}

static void race_gate_nlmsg_end(struct gb_nl_msg* msg,
                                struct nlmsghdr* nlh,
                                struct nlattr* nest_tab,
                                struct nlattr* nest_prio,
                                struct nlattr* nest_opts) {
    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);
    msg->len = nlh->nlmsg_len;
}

static int race_send_bad_clockid(struct gb_nl_sock* sock,
                                 struct gb_nl_msg* msg,
                                 struct gb_nl_msg* resp,
                                 uint32_t index,
                                 int timeout_ms) {
    struct nlmsghdr* nlh;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;

    gb_nl_msg_reset(msg);
    nlh = race_gate_nlmsg_start(msg, NLM_F_CREATE | NLM_F_EXCL, index, &nest_tab, &nest_prio, &nest_opts);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    /* Wrong size for CLOCKID: u64 instead of u32 */
    mnl_attr_put_u64(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, RACE_INVALID_INTERVAL_NS);

    race_gate_nlmsg_end(msg, nlh, nest_tab, nest_prio, nest_opts);
    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static int race_send_bad_base_time(struct gb_nl_sock* sock,
                                   struct gb_nl_msg* msg,
                                   struct gb_nl_msg* resp,
                                   uint32_t index,
                                   int timeout_ms) {
    struct nlmsghdr* nlh;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;

    gb_nl_msg_reset(msg);
    nlh = race_gate_nlmsg_start(msg, NLM_F_CREATE | NLM_F_EXCL, index, &nest_tab, &nest_prio, &nest_opts);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    /* Wrong size for BASE_TIME: u32 instead of u64 */
    mnl_attr_put_u32(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, RACE_INVALID_INTERVAL_NS);

    race_gate_nlmsg_end(msg, nlh, nest_tab, nest_prio, nest_opts);
    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static int race_send_bad_cycle_time(struct gb_nl_sock* sock,
                                    struct gb_nl_msg* msg,
                                    struct gb_nl_msg* resp,
                                    uint32_t index,
                                    int timeout_ms) {
    struct nlmsghdr* nlh;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;

    gb_nl_msg_reset(msg);
    nlh = race_gate_nlmsg_start(msg, NLM_F_CREATE | NLM_F_EXCL, index, &nest_tab, &nest_prio, &nest_opts);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    /* Wrong size for CYCLE_TIME: u32 instead of u64 */
    mnl_attr_put_u32(nlh, TCA_GATE_CYCLE_TIME, RACE_INVALID_INTERVAL_NS);

    race_gate_nlmsg_end(msg, nlh, nest_tab, nest_prio, nest_opts);
    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static int race_send_invalid_action(struct gb_nl_sock* sock,
                                    struct gb_nl_msg* msg,
                                    struct gb_nl_msg* resp,
                                    uint32_t index,
                                    int timeout_ms) {
    struct nlmsghdr* nlh;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;

    gb_nl_msg_reset(msg);
    nlh = race_gate_nlmsg_start(msg, NLM_F_CREATE | NLM_F_EXCL, index, &nest_tab, &nest_prio, &nest_opts);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = 0x7fffffff;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, RACE_INVALID_INTERVAL_NS);

    race_gate_nlmsg_end(msg, nlh, nest_tab, nest_prio, nest_opts);
    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static int race_send_invalid_entry_attr(struct gb_nl_sock* sock,
                                        struct gb_nl_msg* msg,
                                        struct gb_nl_msg* resp,
                                        uint32_t index,
                                        uint32_t which,
                                        int timeout_ms) {
    struct nlmsghdr* nlh;
    struct nlattr *nest_tab, *nest_prio, *nest_opts, *entry_list, *entry_nest;
    struct tc_gate gate_params;
    uint32_t interval = RACE_INVALID_INTERVAL_NS;

    gb_nl_msg_reset(msg);
    nlh = race_gate_nlmsg_start(msg, NLM_F_CREATE | NLM_F_EXCL, index, &nest_tab, &nest_prio, &nest_opts);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, interval);

    entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
    entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);

    switch (which) {
        case 0: {
            size_t before = nlh->nlmsg_len;
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            struct nlattr* attr = (struct nlattr*)((char*)nlh + before);
            attr->nla_len = NLA_HDRLEN + 1;
            break;
        }
        case 1: {
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            size_t before = nlh->nlmsg_len;
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_IPV, 0);
            struct nlattr* attr = (struct nlattr*)((char*)nlh + before);
            attr->nla_len = NLA_HDRLEN + 1;
            break;
        }
        default: {
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            size_t before = nlh->nlmsg_len;
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_MAX_OCTETS, 0);
            struct nlattr* attr = (struct nlattr*)((char*)nlh + before);
            attr->nla_len = NLA_HDRLEN + 1;
            break;
        }
    }

    mnl_attr_nest_end(nlh, entry_nest);
    mnl_attr_nest_end(nlh, entry_list);

    race_gate_nlmsg_end(msg, nlh, nest_tab, nest_prio, nest_opts);
    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static uint32_t race_fill_entries(struct gate_entry* entries,
                                  uint32_t max_entries,
                                  uint32_t interval_max,
                                  uint32_t* seed) {
    uint32_t count = 1u + rng_range(seed, max_entries);

    if (interval_max == 0)
        interval_max = 1;

    for (uint32_t i = 0; i < count; i++) {
        entries[i].index = i;
        entries[i].interval = 1u + rng_range(seed, interval_max);
        entries[i].gate_state = rng_range(seed, 2u) != 0u;
        entries[i].ipv = -1;
        entries[i].maxoctets = race_random_maxoctets(seed);
    }

    return count;
}

static void* race_replace_thread(void* arg) {
    struct gb_race_nl_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* req = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_entry* entries = NULL;
    struct gate_shape shape;
    size_t cap;
    int ret;

    race_pin_thread("replace", ctx->cpu);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        return NULL;
    }

    entries = calloc(ctx->max_entries, sizeof(*entries));
    if (!entries) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        gb_nl_close(sock);
        return NULL;
    }

    cap = gate_msg_capacity(ctx->max_entries, 0);
    req = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!req || !resp) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    race_shape_init(&shape, ctx->cfg);

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        uint32_t count = race_fill_entries(entries, ctx->max_entries, ctx->interval_max, &ctx->seed);

        ret = build_gate_newaction(req, ctx->index, &shape, entries, count, NLM_F_CREATE | NLM_F_REPLACE, 0, -1);
        if (ret < 0) {
            race_record_err(&ctx->errors, ctx->err_counts, ret);
            continue;
        }
        ret = gb_nl_send_recv(sock, req, resp, ctx->timeout_ms);
        if (ret < 0 && ret != -EEXIST && ret != -ENOENT)
            race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    free(entries);
    if (req)
        gb_nl_msg_free(req);
    if (resp)
        gb_nl_msg_free(resp);
    gb_nl_close(sock);
    return NULL;
}

static void* race_dump_thread(void* arg) {
    struct gb_race_dump_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* req = NULL;
    struct gb_dump_stats stats;
    int ret;

    race_pin_thread("dump", ctx->cpu);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        return NULL;
    }

    req = gb_nl_msg_alloc(1024u);
    if (!req) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        gb_nl_close(sock);
        return NULL;
    }

    ret = build_gate_getaction_ex(req, ctx->index, NLM_F_DUMP);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        gb_nl_msg_free(req);
        gb_nl_close(sock);
        return NULL;
    }

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        ret = gb_nl_dump_action(sock, req, &stats, ctx->timeout_ms);
        if (ret < 0)
            race_record_err(&ctx->errors, ctx->err_counts, ret);
        else if (stats.saw_error)
            race_record_err(&ctx->errors, ctx->err_counts, stats.error_code);

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

    gb_nl_msg_free(req);
    gb_nl_close(sock);
    return NULL;
}

static void* race_delete_thread(void* arg) {
    struct gb_race_nl_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* del_msg = NULL;
    struct gb_nl_msg* create_msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_entry* entries = NULL;
    struct gate_shape shape;
    size_t create_cap;
    int ret;

    race_pin_thread("delete", ctx->cpu);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        return NULL;
    }

    entries = calloc(ctx->max_entries, sizeof(*entries));
    if (!entries) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        gb_nl_close(sock);
        return NULL;
    }

    del_msg = gb_nl_msg_alloc(1024u);
    create_cap = gate_msg_capacity(ctx->max_entries, 0);
    create_msg = gb_nl_msg_alloc(create_cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!del_msg || !create_msg || !resp) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    ret = build_gate_delaction(del_msg, ctx->index);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        goto out;
    }
    race_shape_init(&shape, ctx->cfg);

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        ret = gb_nl_send_recv(sock, del_msg, resp, ctx->timeout_ms);
        if (ret < 0 && ret != -ENOENT)
            race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);

        {
            uint32_t count = race_fill_entries(entries, ctx->max_entries, ctx->interval_max, &ctx->seed);
            ret =
                build_gate_newaction(create_msg, ctx->index, &shape, entries, count, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
        }
        if (ret < 0) {
            race_record_err(&ctx->errors, ctx->err_counts, ret);
            continue;
        }
        ret = gb_nl_send_recv(sock, create_msg, resp, ctx->timeout_ms);
        if (ret < 0 && ret != -EEXIST)
            race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);

        ctx->ops++;
        usleep(100);
    }

out:
    free(entries);
    if (del_msg)
        gb_nl_msg_free(del_msg);
    if (create_msg)
        gb_nl_msg_free(create_msg);
    if (resp)
        gb_nl_msg_free(resp);
    gb_nl_close(sock);
    return NULL;
}

static void* race_traffic_thread(void* arg) {
    struct gb_race_traffic_ctx* ctx = arg;
    int fd;
    struct sockaddr_in addr;
    char payload[RACE_MAX_PKT];
    struct timeval tv;
    ssize_t ret;

    race_pin_thread("traffic", ctx->cpu);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, -errno);
        return NULL;
    }

    memset(&tv, 0, sizeof(tv));
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    memset(payload, 0x5a, sizeof(payload));

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        uint32_t span = RACE_MAX_PKT - RACE_MIN_PKT + 1u;
        uint32_t len = RACE_MIN_PKT + rng_range(&ctx->seed, span);

        ret = sendto(fd, payload, len, 0, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0) {
            int err = errno;
            race_record_err(&ctx->errors, ctx->err_counts, -err);
        }
        else
            ctx->ops++;

        if ((ctx->ops & 0xfffu) == 0u)
            usleep(100);
    }

    close(fd);
    return NULL;
}

static void* race_invalid_thread(void* arg) {
    struct gb_race_invalid_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gb_nl_msg* del_msg = NULL;
    uint32_t base_index;
    int ret;

    race_pin_thread("invalid", ctx->cpu);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        return NULL;
    }

    msg = gb_nl_msg_alloc(2048u);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    del_msg = gb_nl_msg_alloc(1024u);
    if (!msg || !resp || !del_msg) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    base_index = ctx->index;

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        uint32_t which = ctx->seed++ % RACE_INVALID_CASES;
        uint32_t index = base_index + which;

        switch (which) {
            case 0:
                ret = race_send_bad_clockid(sock, msg, resp, index, ctx->timeout_ms);
                break;
            case 1:
                ret = race_send_bad_base_time(sock, msg, resp, index, ctx->timeout_ms);
                break;
            case 2:
                ret = race_send_bad_cycle_time(sock, msg, resp, index, ctx->timeout_ms);
                break;
            case 3:
                ret = race_send_invalid_action(sock, msg, resp, index, ctx->timeout_ms);
                break;
            case 4:
                ret = race_send_invalid_entry_attr(sock, msg, resp, index, 0, ctx->timeout_ms);
                break;
            case 5:
                ret = race_send_invalid_entry_attr(sock, msg, resp, index, 1, ctx->timeout_ms);
                break;
            default:
                ret = race_send_invalid_entry_attr(sock, msg, resp, index, 2, ctx->timeout_ms);
                break;
        }

        if (ret < 0)
            race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);
        else {
            gb_nl_msg_reset(del_msg);
            if (build_gate_delaction(del_msg, index) >= 0)
                (void)gb_nl_send_recv(sock, del_msg, resp, ctx->timeout_ms);
        }

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    if (del_msg)
        gb_nl_msg_free(del_msg);
    gb_nl_close(sock);
    return NULL;
}

int gb_race_run(const struct gb_config* cfg) {
    atomic_bool stop = ATOMIC_VAR_INIT(false);
    struct gb_race_nl_ctx replace_ctx;
    struct gb_race_dump_ctx dump_ctx;
    struct gb_race_nl_ctx delete_ctx;
    struct gb_race_traffic_ctx traffic_ctx;
    struct gb_race_invalid_ctx invalid_ctx;
    pthread_t replace_thread;
    pthread_t dump_thread;
    pthread_t delete_thread;
    pthread_t traffic_thread;
    pthread_t invalid_thread;
    int cpus[RACE_THREAD_COUNT];
    int cpu_count;
    uint32_t base_interval;
    uint32_t interval_max;
    uint32_t max_entries;
    uint32_t invalid_base;
    uint64_t sleep_ns;
    int ret;
    int created = 0;

    if (!cfg)
        return -EINVAL;

    if (cfg->race_seconds == 0)
        return -EINVAL;

    max_entries = cfg->entries == 0 ? 1u : cfg->entries;
    if (max_entries > GB_MAX_ENTRIES)
        max_entries = GB_MAX_ENTRIES;
    if (cfg->interval_ns == 0 || cfg->interval_ns > UINT32_MAX)
        base_interval = 1000000u;
    else
        base_interval = (uint32_t)cfg->interval_ns;

    if (base_interval < (UINT32_MAX / 2u))
        interval_max = base_interval * 2u;
    else
        interval_max = UINT32_MAX;

    invalid_base = cfg->index + 0x10000u;
    if (invalid_base < cfg->index)
        invalid_base = cfg->index;

    cpu_count = race_collect_cpus(cpus, (int)RACE_THREAD_COUNT);
    if (cpu_count <= 0) {
        for (unsigned int i = 0; i < RACE_THREAD_COUNT; i++)
            cpus[i] = -1;
        cpu_count = 0;
    }

    replace_ctx = (struct gb_race_nl_ctx){
        .cfg = cfg,
        .stop = &stop,
        .seed = RACE_SEED_BASE ^ 0x11111111u,
        .index = cfg->index,
        .max_entries = max_entries,
        .interval_max = interval_max,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[0 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    dump_ctx = (struct gb_race_dump_ctx){
        .cfg = cfg,
        .stop = &stop,
        .index = cfg->index,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[1 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    delete_ctx = (struct gb_race_nl_ctx){
        .cfg = cfg,
        .stop = &stop,
        .seed = RACE_SEED_BASE ^ 0x33333333u,
        .index = cfg->index,
        .max_entries = max_entries,
        .interval_max = interval_max,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[3 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    traffic_ctx = (struct gb_race_traffic_ctx){
        .stop = &stop,
        .seed = RACE_SEED_BASE ^ 0x77777777u,
        .cpu = cpu_count > 0 ? cpus[2 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    invalid_ctx = (struct gb_race_invalid_ctx){
        .cfg = cfg,
        .stop = &stop,
        .seed = RACE_SEED_BASE ^ 0x99999999u,
        .index = invalid_base,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[4 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    if (!cfg->json) {
        if (cpu_count < (int)RACE_THREAD_COUNT) {
            printf("Note: only %d CPU%s available; race threads will share CPUs\n", cpu_count,
                   cpu_count == 1 ? "" : "s");
        }
        printf("Race thread CPUs: replace=%d dump=%d traffic=%d delete=%d invalid=%d\n", replace_ctx.cpu, dump_ctx.cpu,
               traffic_ctx.cpu, delete_ctx.cpu, invalid_ctx.cpu);
    }

    ret = pthread_create(&replace_thread, NULL, race_replace_thread, &replace_ctx);
    if (ret != 0)
        return -ret;
    created++;

    ret = pthread_create(&dump_thread, NULL, race_dump_thread, &dump_ctx);
    if (ret != 0)
        goto out_stop;
    created++;

    ret = pthread_create(&traffic_thread, NULL, race_traffic_thread, &traffic_ctx);
    if (ret != 0)
        goto out_stop;
    created++;

    ret = pthread_create(&delete_thread, NULL, race_delete_thread, &delete_ctx);
    if (ret != 0)
        goto out_stop;
    created++;

    ret = pthread_create(&invalid_thread, NULL, race_invalid_thread, &invalid_ctx);
    if (ret != 0)
        goto out_stop;
    created++;

    sleep_ns = (uint64_t)cfg->race_seconds * 1000000000ull;
    (void)gb_util_sleep_ns(sleep_ns);

out_stop:
    atomic_store_explicit(&stop, true, memory_order_relaxed);

    if (created > 0)
        pthread_join(replace_thread, NULL);
    if (created > 1)
        pthread_join(dump_thread, NULL);
    if (created > 2)
        pthread_join(traffic_thread, NULL);
    if (created > 3)
        pthread_join(delete_thread, NULL);
    if (created > 4)
        pthread_join(invalid_thread, NULL);

    if (!cfg->json) {
        if (ret == 0) {
            printf("Race mode completed (%u seconds)\n", cfg->race_seconds);
        }
        else {
            printf("Race mode stopped early: %s (%d)\n", strerror(ret), ret);
        }
        printf("  Replace ops: %llu, errors: %llu\n", (unsigned long long)replace_ctx.ops,
               (unsigned long long)replace_ctx.errors);
        printf("  Dump ops:    %llu, errors: %llu\n", (unsigned long long)dump_ctx.ops,
               (unsigned long long)dump_ctx.errors);
        printf("  Traffic ops: %llu, errors: %llu\n", (unsigned long long)traffic_ctx.ops,
               (unsigned long long)traffic_ctx.errors);
        printf("  Delete ops:  %llu, errors: %llu\n", (unsigned long long)delete_ctx.ops,
               (unsigned long long)delete_ctx.errors);
        printf("  Invalid ops: %llu, errors: %llu\n", (unsigned long long)invalid_ctx.ops,
               (unsigned long long)invalid_ctx.errors);
        race_print_err_breakdown("Replace", replace_ctx.errors, replace_ctx.err_counts);
        race_print_err_breakdown("Dump", dump_ctx.errors, dump_ctx.err_counts);
        race_print_err_breakdown("Traffic", traffic_ctx.errors, traffic_ctx.err_counts);
        race_print_err_breakdown("Delete", delete_ctx.errors, delete_ctx.err_counts);
        race_print_err_breakdown("Invalid", invalid_ctx.errors, invalid_ctx.err_counts);
        race_print_extack("Replace", &replace_ctx.extack);
        race_print_extack("Dump", &dump_ctx.extack);
        race_print_extack("Delete", &delete_ctx.extack);
        race_print_extack("Invalid", &invalid_ctx.extack);
    }

    if (ret != 0)
        return -ret;
    return 0;
}
