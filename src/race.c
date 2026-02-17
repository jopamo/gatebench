/* src/race.c
 * Race mode workload generation.
 */
#include "../include/gatebench_race.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"
#include "../include/gatebench_util.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-format-attribute"
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wfloat-conversion"
#pragma clang diagnostic ignored "-Wimplicit-float-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-format-attribute"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include "../include/tst_fuzzy_sync.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RACE_SEED_BASE 0x5a17c3d1u
#define RACE_MIN_PKT 64u
#define RACE_MAX_PKT 1500u
#define RACE_INVALID_INTERVAL_NS 1000000u
#define RACE_ERRNO_MAX 4096u
#define RACE_EXTACK_MSG_MAX 128u
#define RACE_EXTACK_SLOTS 6u
#define RACE_INVALID_CASES 8u
#define RACE_BASETIME_JITTER_NS 10000000u
#define RACE_THREAD_COUNT GB_RACE_THREAD_COUNT
#define RACE_PAIR_COUNT (RACE_THREAD_COUNT / 2u)
#define RACE_PAIR_SWAP_SLICE_NS 1000000000ull

#ifndef NLM_F_ACK_TLVS
#define NLM_F_ACK_TLVS 0x200
#endif

#ifndef NLMSGERR_ATTR_MSG
#define NLMSGERR_ATTR_MSG 1
#endif

_Static_assert((RACE_THREAD_COUNT % 2u) == 0u, "RACE_THREAD_COUNT must be even for pair shuffling");

struct race_sync_profile {
    float alpha;
    int min_samples;
    float max_dev_ratio;
};

static const char* const race_worker_names[RACE_THREAD_COUNT] = {
    "replace", "dump", "get", "traffic", "basetime", "delete", "invalid", "traffic_sync",
};

static const struct race_sync_profile race_worker_profiles[RACE_THREAD_COUNT] = {
    {0.30f, 256, 0.15f}, {0.25f, 192, 0.15f}, {0.25f, 192, 0.15f}, {0.25f, 192, 0.20f},
    {0.30f, 256, 0.15f}, {0.30f, 256, 0.15f}, {0.30f, 256, 0.15f}, {0.25f, 192, 0.20f},
};

static void race_set_attr_len_unaligned(struct nlmsghdr* nlh, size_t attr_offset, uint16_t attr_len) {
    memcpy((char*)nlh + attr_offset + offsetof(struct nlattr, nla_len), &attr_len, sizeof(attr_len));
}

struct gb_race_extack_entry {
    char msg[RACE_EXTACK_MSG_MAX];
    uint64_t count;
};

struct gb_race_extack_stats {
    struct gb_race_extack_entry entries[RACE_EXTACK_SLOTS];
    uint64_t other;
};

struct race_extack_parse_ctx {
    char* out;
    size_t out_len;
    bool found;
};

struct gb_race_nl_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    struct tst_fzsync_pair* sync_pair;
    bool sync_is_a;
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
    struct tst_fzsync_pair* sync_pair;
    bool sync_is_a;
    uint32_t index;
    int timeout_ms;
    int cpu;
    uint64_t ops;
    uint64_t errors;
    uint64_t err_counts[RACE_ERRNO_MAX];
    struct gb_race_extack_stats extack;
};

struct gb_race_get_ctx {
    atomic_bool* stop;
    struct tst_fzsync_pair* sync_pair;
    bool sync_is_a;
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
    struct tst_fzsync_pair* sync_pair;
    bool sync_is_a;
    uint32_t seed;
    int cpu;
    uint64_t ops;
    uint64_t errors;
    uint64_t err_counts[RACE_ERRNO_MAX];
};

struct gb_race_sync_ctx {
    atomic_bool* stop;
    struct tst_fzsync_pair* sync_pair;
    bool sync_is_a;
    int cpu;
    uint64_t ops;
};

struct gb_race_invalid_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    struct tst_fzsync_pair* sync_pair;
    bool sync_is_a;
    uint32_t seed;
    uint32_t index;
    uint32_t live_index;
    int timeout_ms;
    int cpu;
    uint64_t ops;
    uint64_t errors;
    uint64_t err_counts[RACE_ERRNO_MAX];
    struct gb_race_extack_stats extack;
};

struct gb_race_update_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    struct tst_fzsync_pair* sync_pair;
    bool sync_is_a;
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

static int race_extack_attr_cb(const struct nlattr* attr, void* data) {
    struct race_extack_parse_ctx* ctx = data;
    const char* msg;

    if (!ctx || ctx->found)
        return MNL_CB_OK;

    if (mnl_attr_get_type(attr) != NLMSGERR_ATTR_MSG)
        return MNL_CB_OK;

    if (mnl_attr_get_payload_len(attr) == 0)
        return MNL_CB_OK;

    msg = mnl_attr_get_str(attr);
    if (!msg || msg[0] == '\0')
        return MNL_CB_OK;

    (void)snprintf(ctx->out, ctx->out_len, "%s", msg);
    ctx->found = true;
    return MNL_CB_STOP;
}

static bool race_parse_extack_msg(const struct gb_nl_msg* resp, char* out, size_t out_len) {
    const struct nlmsghdr* nlh;
    struct race_extack_parse_ctx ctx;
    size_t payload_len;
    size_t err_len;
    size_t attr_offset_sz;
    unsigned int attr_offset;

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

    attr_offset_sz = (size_t)NLMSG_HDRLEN + err_len;
    if (attr_offset_sz > (size_t)UINT_MAX)
        return false;

    attr_offset = (unsigned int)attr_offset_sz;
    out[0] = '\0';
    ctx.out = out;
    ctx.out_len = out_len;
    ctx.found = false;

    if (mnl_attr_parse(nlh, attr_offset, race_extack_attr_cb, &ctx) < 0)
        return false;

    return ctx.found;
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
        size_t copy_len = strnlen(msg, sizeof(stats->entries[empty].msg) - 1u);
        memcpy(stats->entries[empty].msg, msg, copy_len);
        stats->entries[empty].msg[copy_len] = '\0';
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
    int nproc_i;

    if (!cpus || max <= 0)
        return 0;

    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (size_t cpu = 0; cpu < (size_t)CPU_SETSIZE && count < max; cpu++) {
            if (CPU_ISSET(cpu, &set))
                cpus[count++] = (int)cpu;
        }
        if (count > 0)
            return count;
    }

    nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0)
        return 0;

    if (nproc > max)
        nproc = max;

    nproc_i = (int)nproc;
    for (int i = 0; i < nproc_i; i++)
        cpus[i] = i;

    return nproc_i;
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

static uint64_t race_clock_now_ns(clockid_t clockid) {
    struct timespec ts;

    if (clock_gettime(clockid, &ts) != 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
            return 0;
    }

    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
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
    mnl_attr_put_strz(nlh, TCA_ACT_KIND, "gate");
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
    const uint16_t bad_attr_len = (uint16_t)(sizeof(struct nlattr) + 1u);

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
            race_set_attr_len_unaligned(nlh, before, bad_attr_len);
            break;
        }
        case 1: {
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            size_t before = nlh->nlmsg_len;
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_IPV, 0);
            race_set_attr_len_unaligned(nlh, before, bad_attr_len);
            break;
        }
        default: {
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            size_t before = nlh->nlmsg_len;
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_MAX_OCTETS, 0);
            race_set_attr_len_unaligned(nlh, before, bad_attr_len);
            break;
        }
    }

    mnl_attr_nest_end(nlh, entry_nest);
    mnl_attr_nest_end(nlh, entry_list);

    race_gate_nlmsg_end(msg, nlh, nest_tab, nest_prio, nest_opts);
    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static int race_send_bad_interval(struct gb_nl_sock* sock,
                                  struct gb_nl_msg* msg,
                                  struct gb_nl_msg* resp,
                                  uint32_t index,
                                  int timeout_ms) {
    struct gate_shape shape;
    struct gate_entry entry;
    int ret;

    memset(&shape, 0, sizeof(shape));
    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = RACE_INVALID_INTERVAL_NS;
    shape.cycle_time_ext = 0;
    shape.interval_ns = RACE_INVALID_INTERVAL_NS;
    shape.entries = 1;

    memset(&entry, 0, sizeof(entry));
    entry.index = 0;
    entry.gate_state = true;
    entry.interval = RACE_INVALID_INTERVAL_NS;
    entry.ipv = -1;
    entry.maxoctets = -1;

    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0)
        return ret;

    ret = gb_nl_send_recv(sock, msg, resp, timeout_ms);
    if (ret < 0 && ret != -EEXIST)
        return ret;

    entry.interval = 0; /* invalid interval on replace */
    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, index, &shape, &entry, 1, NLM_F_REPLACE, 0, -1);
    if (ret < 0)
        return ret;

    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static int race_send_basetime_update(struct gb_nl_sock* sock,
                                     struct gb_nl_msg* msg,
                                     struct gb_nl_msg* resp,
                                     const struct gb_config* cfg,
                                     uint32_t index,
                                     uint64_t basetime,
                                     uint32_t clockid,
                                     const struct gate_entry* entries,
                                     uint32_t num_entries,
                                     int timeout_ms) {
    struct gate_shape shape;
    int ret;

    memset(&shape, 0, sizeof(shape));
    shape.clockid = clockid;
    shape.base_time = basetime;

    /* Prefer configured cycle_time; otherwise derive from current entry list. */
    shape.cycle_time = cfg ? cfg->cycle_time : 0;
    if (shape.cycle_time == 0 && entries && num_entries > 0) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < num_entries; i++)
            sum += (uint64_t)entries[i].interval;
        shape.cycle_time = sum;
    }
    if (shape.cycle_time == 0) {
        uint64_t interval_ns = cfg ? cfg->interval_ns : 0;
        shape.cycle_time = interval_ns ? interval_ns : 1000000ull;
    }
    shape.cycle_time_ext = cfg ? cfg->cycle_time_ext : 0;
    if (timeout_ms < 0)
        timeout_ms = 0;

    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, index, &shape, entries, num_entries, NLM_F_REPLACE, 0, -1);
    if (ret < 0)
        return ret;

    return gb_nl_send_recv(sock, msg, resp, timeout_ms);
}

static int race_send_timerstart_replace_live(struct gb_nl_sock* sock,
                                             struct gb_nl_msg* msg,
                                             struct gb_nl_msg* resp,
                                             const struct gb_config* cfg,
                                             uint32_t index,
                                             uint32_t* seed,
                                             int timeout_ms) {
    struct gate_dump dump;
    uint32_t clockid;
    uint64_t now;
    uint64_t basetime;
    int ret;

    if (!seed)
        return -EINVAL;

    memset(&dump, 0, sizeof(dump));
    ret = gb_nl_get_action(sock, index, &dump, timeout_ms);
    if (ret < 0)
        return ret;

    if (!dump.entries || dump.num_entries == 0u) {
        gb_gate_dump_free(&dump);
        return -ENOENT;
    }
    if (dump.num_entries > GB_MAX_ENTRIES) {
        gb_gate_dump_free(&dump);
        return -E2BIG;
    }

    clockid = (rng_range(seed, 2u) == 0u) ? CLOCK_TAI : CLOCK_MONOTONIC;
    now = race_clock_now_ns((clockid_t)clockid);
    basetime = now + 1u + (uint64_t)rng_range(seed, RACE_BASETIME_JITTER_NS);

    ret = race_send_basetime_update(sock, msg, resp, cfg, index, basetime, clockid, dump.entries, dump.num_entries,
                                    timeout_ms);
    gb_gate_dump_free(&dump);
    return ret;
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

static bool race_sync_exit_requested(const struct tst_fzsync_pair* pair) {
    if (!pair)
        return false;
    return tst_atomic_load(&pair->exit) != 0;
}

static void race_sync_signal_exit(struct tst_fzsync_pair* pair) {
    if (!pair)
        return;
    tst_atomic_store(1, &pair->exit);
}

static void race_sync_start(struct tst_fzsync_pair* pair, bool is_a) {
    if (!pair)
        return;

    if (is_a)
        tst_fzsync_start_race_a(pair);
    else
        tst_fzsync_start_race_b(pair);
}

static void race_sync_end(struct tst_fzsync_pair* pair, bool is_a) {
    if (!pair)
        return;

    if (is_a)
        tst_fzsync_end_race_a(pair);
    else
        tst_fzsync_end_race_b(pair);
}

static void race_sync_pair_init(struct tst_fzsync_pair* pair, float alpha, int min_samples, float max_dev_ratio) {
    if (!pair)
        return;

    memset(pair, 0, sizeof(*pair));
    pair->avg_alpha = alpha;
    pair->min_samples = min_samples;
    pair->max_dev_ratio = max_dev_ratio;
    pair->exec_loops = INT_MAX;
    tst_fzsync_pair_init(pair);
    tst_fzsync_pair_reset(pair, NULL);
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
        goto out;
    }

    entries = calloc(ctx->max_entries, sizeof(*entries));
    if (!entries) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    cap = gate_msg_capacity(ctx->max_entries, 0);
    req = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!req || !resp) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    race_shape_init(&shape, ctx->cfg);

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        uint32_t count = race_fill_entries(entries, ctx->max_entries, ctx->interval_max, &ctx->seed);

        ret = build_gate_newaction(req, ctx->index, &shape, entries, count, NLM_F_CREATE | NLM_F_REPLACE, 0, -1);
        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        if (ret < 0)
            race_record_err(&ctx->errors, ctx->err_counts, ret);
        else {
            ret = gb_nl_send_recv(sock, req, resp, ctx->timeout_ms);
            if (ret < 0 && ret != -EEXIST && ret != -ENOENT)
                race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);
        }
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    race_sync_signal_exit(ctx->sync_pair);
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
        goto out;
    }

    req = gb_nl_msg_alloc(1024u);
    if (!req) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    ret = build_gate_getaction_ex(req, ctx->index, NLM_F_DUMP);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        goto out;
    }

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        ret = gb_nl_dump_action(sock, req, &stats, ctx->timeout_ms);
        if (ret < 0)
            race_record_err(&ctx->errors, ctx->err_counts, ret);
        else if (stats.saw_error)
            race_record_err(&ctx->errors, ctx->err_counts, stats.error_code);
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    race_sync_signal_exit(ctx->sync_pair);
    if (req)
        gb_nl_msg_free(req);
    gb_nl_close(sock);
    return NULL;
}

static void* race_get_thread(void* arg) {
    struct gb_race_get_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* req = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_dump dump;
    int ret;

    race_pin_thread("get", ctx->cpu);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        goto out;
    }

    req = gb_nl_msg_alloc(1024u);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!req || !resp) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    ret = build_gate_getaction(req, ctx->index);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        goto out;
    }

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        ret = gb_nl_send_recv(sock, req, resp, ctx->timeout_ms);
        if (ret < 0) {
            if (ret != -ENOENT)
                race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);
        }
        else {
            ret = gb_nl_gate_parse((struct nlmsghdr*)resp->buf, &dump);
            if (ret < 0) {
                race_record_err(&ctx->errors, ctx->err_counts, ret);
                gb_gate_dump_free(&dump);
            }
            else {
                gb_gate_dump_free(&dump);
            }
        }
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    race_sync_signal_exit(ctx->sync_pair);
    if (req)
        gb_nl_msg_free(req);
    if (resp)
        gb_nl_msg_free(resp);
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
        goto out;
    }

    entries = calloc(ctx->max_entries, sizeof(*entries));
    if (!entries) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
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

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        ret = gb_nl_send_recv(sock, del_msg, resp, ctx->timeout_ms);
        if (ret < 0 && ret != -ENOENT)
            race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

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
    race_sync_signal_exit(ctx->sync_pair);
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
    int fd = -1;
    struct sockaddr_in addr;
    char payload[RACE_MAX_PKT];
    struct timeval tv;
    ssize_t ret;

    race_pin_thread("traffic", ctx->cpu);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, -errno);
        goto out;
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

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        uint32_t span = RACE_MAX_PKT - RACE_MIN_PKT + 1u;
        uint32_t len = RACE_MIN_PKT + rng_range(&ctx->seed, span);

        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        ret = sendto(fd, payload, len, 0, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0) {
            int err = errno;
            race_record_err(&ctx->errors, ctx->err_counts, -err);
        }
        else
            ctx->ops++;
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

        if ((ctx->ops & 0xfffu) == 0u)
            usleep(100);
    }

out:
    race_sync_signal_exit(ctx->sync_pair);
    if (fd >= 0)
        close(fd);
    return NULL;
}

static void* race_sync_partner_thread(void* arg) {
    struct gb_race_sync_ctx* ctx = arg;
    volatile unsigned int spin = 0;

    race_pin_thread("traffic_sync", ctx->cpu);

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        for (unsigned int i = 0; i < 64u; i++)
            spin += i;
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

        ctx->ops++;
        if ((ctx->ops & 0x3ffu) == 0u)
            sched_yield();
    }

    race_sync_signal_exit(ctx->sync_pair);
    (void)spin;
    return NULL;
}

static void* race_invalid_thread(void* arg) {
    struct gb_race_invalid_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gb_nl_msg* del_msg = NULL;
    uint32_t base_index;
    size_t msg_cap;
    int ret;

    race_pin_thread("invalid", ctx->cpu);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        goto out;
    }

    /*
     * This thread now issues live REPLACE updates that may include the full
     * current schedule; size the request buffer accordingly.
     */
    msg_cap = gate_msg_capacity(GB_MAX_ENTRIES, 0);
    msg = gb_nl_msg_alloc(msg_cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    del_msg = gb_nl_msg_alloc(1024u);
    if (!msg || !resp || !del_msg) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    base_index = ctx->index;

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        if ((ctx->ops & 1u) == 0u) {
            ret = race_send_timerstart_replace_live(sock, msg, resp, ctx->cfg, ctx->live_index, &ctx->seed,
                                                    ctx->timeout_ms);
            if (ret < 0 && ret != -ENOENT)
                race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);
        }
        else {
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
                case 6:
                    ret = race_send_invalid_entry_attr(sock, msg, resp, index, 2, ctx->timeout_ms);
                    break;
                default:
                    ret = race_send_bad_interval(sock, msg, resp, index, ctx->timeout_ms);
                    break;
            }

            if (ret < 0)
                race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);

            gb_nl_msg_reset(del_msg);
            if (build_gate_delaction(del_msg, index) >= 0)
                (void)gb_nl_send_recv(sock, del_msg, resp, ctx->timeout_ms);
        }
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    race_sync_signal_exit(ctx->sync_pair);
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    if (del_msg)
        gb_nl_msg_free(del_msg);
    gb_nl_close(sock);
    return NULL;
}

static void* race_basetime_thread(void* arg) {
    struct gb_race_update_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_dump dump;
    size_t cap;
    int ret;

    race_pin_thread("basetime", ctx->cpu);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        race_record_err(&ctx->errors, ctx->err_counts, ret);
        goto out;
    }

    cap = gate_msg_capacity(GB_MAX_ENTRIES, 0);
    msg = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!msg || !resp) {
        race_record_err(&ctx->errors, ctx->err_counts, -ENOMEM);
        goto out;
    }

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed) && !race_sync_exit_requested(ctx->sync_pair)) {
        race_sync_start(ctx->sync_pair, ctx->sync_is_a);
        uint64_t now = race_clock_now_ns((clockid_t)ctx->cfg->clockid);
        uint64_t jitter = 1u + (uint64_t)rng_range(&ctx->seed, RACE_BASETIME_JITTER_NS);
        uint64_t basetime = now + jitter;

        /*
         * On some kernels, REPLACE without an entry list is treated as "set an
         * empty list", yielding -EINVAL with extack "The entry list is empty".
         * Avoid that by fetching the current schedule and sending it back with
         * the updated base_time.
         */
        memset(&dump, 0, sizeof(dump));
        ret = gb_nl_get_action(sock, ctx->index, &dump, ctx->timeout_ms);
        if (ret < 0) {
            if (ret != -ENOENT)
                race_record_err(&ctx->errors, ctx->err_counts, ret);
        }
        else {
            ret = race_send_basetime_update(sock, msg, resp, ctx->cfg, ctx->index, basetime, ctx->cfg->clockid,
                                            dump.entries, dump.num_entries, ctx->timeout_ms);
            gb_gate_dump_free(&dump);

            if (ret < 0) {
                if (ret != -ENOENT)
                    race_record_nl_error(&ctx->errors, ctx->err_counts, &ctx->extack, ret, resp);
            }
        }
        race_sync_end(ctx->sync_pair, ctx->sync_is_a);

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    race_sync_signal_exit(ctx->sync_pair);
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    gb_nl_close(sock);
    return NULL;
}

int gb_race_run_with_summary(const struct gb_config* cfg, struct gb_race_summary* summary) {
    atomic_bool stop = ATOMIC_VAR_INIT(false);
    struct gb_race_nl_ctx replace_ctx;
    struct gb_race_dump_ctx dump_ctx;
    struct gb_race_get_ctx get_ctx;
    struct gb_race_nl_ctx delete_ctx;
    struct gb_race_traffic_ctx traffic_ctx;
    struct gb_race_sync_ctx traffic_sync_ctx;
    struct gb_race_invalid_ctx invalid_ctx;
    struct gb_race_update_ctx basetime_ctx;
    struct tst_fzsync_pair sync_pairs[RACE_PAIR_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];
    int cpus[RACE_THREAD_COUNT];
    int cpu_count;
    uint32_t base_interval;
    uint32_t interval_max;
    uint32_t max_entries;
    uint32_t invalid_base;
    uint32_t pair_seed;
    uint64_t total_ns;
    uint64_t remaining_ns;
    unsigned int phase_total;
    unsigned int phase = 0;
    int ret = 0;

    if (summary)
        memset(summary, 0, sizeof(*summary));

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

    gb_fzsync_seed(RACE_SEED_BASE ^ cfg->index ^ cfg->race_seconds);
    gb_fzsync_set_info(cfg->verbose && !cfg->json);

    cpu_count = race_collect_cpus(cpus, (int)RACE_THREAD_COUNT);
    if (cpu_count <= 0) {
        for (unsigned int i = 0; i < RACE_THREAD_COUNT; i++)
            cpus[i] = -1;
        cpu_count = 0;
    }

    replace_ctx = (struct gb_race_nl_ctx){
        .cfg = cfg,
        .stop = &stop,
        .sync_pair = NULL,
        .sync_is_a = true,
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
        .sync_pair = NULL,
        .sync_is_a = true,
        .index = cfg->index,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[1 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    get_ctx = (struct gb_race_get_ctx){
        .stop = &stop,
        .sync_pair = NULL,
        .sync_is_a = false,
        .index = cfg->index,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[2 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    delete_ctx = (struct gb_race_nl_ctx){
        .cfg = cfg,
        .stop = &stop,
        .sync_pair = NULL,
        .sync_is_a = false,
        .seed = RACE_SEED_BASE ^ 0x33333333u,
        .index = cfg->index,
        .max_entries = max_entries,
        .interval_max = interval_max,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[5 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    traffic_ctx = (struct gb_race_traffic_ctx){
        .stop = &stop,
        .sync_pair = NULL,
        .sync_is_a = true,
        .seed = RACE_SEED_BASE ^ 0x77777777u,
        .cpu = cpu_count > 0 ? cpus[3 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    traffic_sync_ctx = (struct gb_race_sync_ctx){
        .stop = &stop,
        .sync_pair = NULL,
        .sync_is_a = false,
        .cpu = cpu_count > 0 ? cpus[7 % cpu_count] : -1,
        .ops = 0,
    };

    invalid_ctx = (struct gb_race_invalid_ctx){
        .cfg = cfg,
        .stop = &stop,
        .sync_pair = NULL,
        .sync_is_a = false,
        .seed = RACE_SEED_BASE ^ 0x99999999u,
        .index = invalid_base,
        .live_index = cfg->index,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[6 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    basetime_ctx = (struct gb_race_update_ctx){
        .cfg = cfg,
        .stop = &stop,
        .sync_pair = NULL,
        .sync_is_a = true,
        .seed = RACE_SEED_BASE ^ 0x55555555u,
        .index = cfg->index,
        .timeout_ms = cfg->timeout_ms,
        .cpu = cpu_count > 0 ? cpus[4 % cpu_count] : -1,
        .ops = 0,
        .errors = 0,
    };

    {
        struct tst_fzsync_pair** const worker_pair_refs[RACE_THREAD_COUNT] = {
            &replace_ctx.sync_pair,  &dump_ctx.sync_pair,   &get_ctx.sync_pair,     &traffic_ctx.sync_pair,
            &basetime_ctx.sync_pair, &delete_ctx.sync_pair, &invalid_ctx.sync_pair, &traffic_sync_ctx.sync_pair,
        };
        bool* const worker_side_refs[RACE_THREAD_COUNT] = {
            &replace_ctx.sync_is_a,  &dump_ctx.sync_is_a,   &get_ctx.sync_is_a,     &traffic_ctx.sync_is_a,
            &basetime_ctx.sync_is_a, &delete_ctx.sync_is_a, &invalid_ctx.sync_is_a, &traffic_sync_ctx.sync_is_a,
        };
        void* (*const worker_fns[RACE_THREAD_COUNT])(void*) = {
            race_replace_thread,  race_dump_thread,   race_get_thread,     race_traffic_thread,
            race_basetime_thread, race_delete_thread, race_invalid_thread, race_sync_partner_thread,
        };
        void* const worker_args[RACE_THREAD_COUNT] = {
            &replace_ctx,  &dump_ctx,   &get_ctx,     &traffic_ctx,
            &basetime_ctx, &delete_ctx, &invalid_ctx, &traffic_sync_ctx,
        };

        total_ns = (uint64_t)cfg->race_seconds * 1000000000ull;
        remaining_ns = total_ns;
        phase_total = (unsigned int)((total_ns + RACE_PAIR_SWAP_SLICE_NS - 1ull) / RACE_PAIR_SWAP_SLICE_NS);
        pair_seed = RACE_SEED_BASE ^ cfg->index ^ cfg->race_seconds ^ 0x9e3779b9u;

        if (!cfg->json) {
            if (cpu_count < (int)RACE_THREAD_COUNT) {
                printf("Note: only %d CPU%s available; race threads will share CPUs\n", cpu_count,
                       cpu_count == 1 ? "" : "s");
            }
            printf(
                "Race thread CPUs: replace=%d dump=%d get=%d traffic=%d basetime=%d delete=%d invalid=%d "
                "traffic_sync=%d\n",
                replace_ctx.cpu, dump_ctx.cpu, get_ctx.cpu, traffic_ctx.cpu, basetime_ctx.cpu, delete_ctx.cpu,
                invalid_ctx.cpu, traffic_sync_ctx.cpu);
            printf("Race fuzzy sync: dynamic pair shuffling enabled (swap interval: %llu ms)\n",
                   (unsigned long long)(RACE_PAIR_SWAP_SLICE_NS / 1000000ull));
            printf("Race invalid thread: valid REPLACE timer-start trigger targets live index %u\n",
                   invalid_ctx.live_index);
        }

        while (remaining_ns > 0 && ret == 0) {
            unsigned int order[RACE_THREAD_COUNT];
            unsigned int pair_members[RACE_PAIR_COUNT][2];
            bool pair_member_is_a[RACE_PAIR_COUNT][2];
            uint64_t phase_ns = remaining_ns > RACE_PAIR_SWAP_SLICE_NS ? RACE_PAIR_SWAP_SLICE_NS : remaining_ns;
            unsigned int created = 0;

            for (unsigned int i = 0; i < RACE_THREAD_COUNT; i++)
                order[i] = i;

            for (unsigned int i = RACE_THREAD_COUNT - 1u; i > 0u; i--) {
                unsigned int j = rng_range(&pair_seed, i + 1u);
                unsigned int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }

            for (unsigned int pair_idx = 0; pair_idx < RACE_PAIR_COUNT; pair_idx++) {
                unsigned int first = order[pair_idx * 2u];
                unsigned int second = order[pair_idx * 2u + 1u];
                const struct race_sync_profile* first_profile = &race_worker_profiles[first];
                const struct race_sync_profile* second_profile = &race_worker_profiles[second];
                float alpha = (first_profile->alpha + second_profile->alpha) * 0.5f;
                int min_samples = (first_profile->min_samples + second_profile->min_samples) / 2;
                float max_dev_ratio = first_profile->max_dev_ratio > second_profile->max_dev_ratio
                                          ? first_profile->max_dev_ratio
                                          : second_profile->max_dev_ratio;
                bool first_is_a = rng_range(&pair_seed, 2u) == 0u;

                race_sync_pair_init(&sync_pairs[pair_idx], alpha, min_samples, max_dev_ratio);
                *worker_pair_refs[first] = &sync_pairs[pair_idx];
                *worker_side_refs[first] = first_is_a;
                *worker_pair_refs[second] = &sync_pairs[pair_idx];
                *worker_side_refs[second] = !first_is_a;
                pair_members[pair_idx][0] = first;
                pair_members[pair_idx][1] = second;
                pair_member_is_a[pair_idx][0] = first_is_a;
                pair_member_is_a[pair_idx][1] = !first_is_a;
            }

            if (!cfg->json && cfg->verbose) {
                printf("Race fuzzy sync phase %u/%u:", phase + 1u, phase_total);
                for (unsigned int pair_idx = 0; pair_idx < RACE_PAIR_COUNT; pair_idx++) {
                    unsigned int first = pair_members[pair_idx][0];
                    unsigned int second = pair_members[pair_idx][1];
                    printf(" [%s(%c)<->%s(%c)]", race_worker_names[first], pair_member_is_a[pair_idx][0] ? 'A' : 'B',
                           race_worker_names[second], pair_member_is_a[pair_idx][1] ? 'A' : 'B');
                }
                printf("\n");
            }

            atomic_store_explicit(&stop, false, memory_order_relaxed);

            for (unsigned int i = 0; i < RACE_THREAD_COUNT; i++) {
                ret = pthread_create(&threads[i], NULL, worker_fns[i], worker_args[i]);
                if (ret != 0)
                    break;
                created++;
            }

            if (ret == 0)
                (void)gb_util_sleep_ns(phase_ns);

            atomic_store_explicit(&stop, true, memory_order_relaxed);
            for (unsigned int i = 0; i < RACE_PAIR_COUNT; i++)
                race_sync_signal_exit(&sync_pairs[i]);

            for (unsigned int i = 0; i < created; i++)
                pthread_join(threads[i], NULL);
            for (unsigned int i = 0; i < RACE_PAIR_COUNT; i++)
                tst_fzsync_pair_cleanup(&sync_pairs[i]);

            if (ret != 0)
                break;

            remaining_ns -= phase_ns;
            phase++;
        }
    }

    if (summary) {
        summary->completed = ret == 0;
        summary->duration_seconds = cfg->race_seconds;
        summary->cpu_count = cpu_count;

        summary->replace.cpu = replace_ctx.cpu;
        summary->replace.ops = replace_ctx.ops;
        summary->replace.errors = replace_ctx.errors;

        summary->dump.cpu = dump_ctx.cpu;
        summary->dump.ops = dump_ctx.ops;
        summary->dump.errors = dump_ctx.errors;

        summary->get.cpu = get_ctx.cpu;
        summary->get.ops = get_ctx.ops;
        summary->get.errors = get_ctx.errors;

        summary->traffic.cpu = traffic_ctx.cpu;
        summary->traffic.ops = traffic_ctx.ops;
        summary->traffic.errors = traffic_ctx.errors;

        summary->traffic_sync.cpu = traffic_sync_ctx.cpu;
        summary->traffic_sync.ops = traffic_sync_ctx.ops;

        summary->basetime.cpu = basetime_ctx.cpu;
        summary->basetime.ops = basetime_ctx.ops;
        summary->basetime.errors = basetime_ctx.errors;

        summary->delete_worker.cpu = delete_ctx.cpu;
        summary->delete_worker.ops = delete_ctx.ops;
        summary->delete_worker.errors = delete_ctx.errors;

        summary->invalid.cpu = invalid_ctx.cpu;
        summary->invalid.ops = invalid_ctx.ops;
        summary->invalid.errors = invalid_ctx.errors;
    }

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
        printf("  Get ops:     %llu, errors: %llu\n", (unsigned long long)get_ctx.ops,
               (unsigned long long)get_ctx.errors);
        printf("  Traffic ops: %llu, errors: %llu\n", (unsigned long long)traffic_ctx.ops,
               (unsigned long long)traffic_ctx.errors);
        printf("  Traffic sync ops:%llu\n", (unsigned long long)traffic_sync_ctx.ops);
        printf("  Basetime ops:%llu, errors: %llu\n", (unsigned long long)basetime_ctx.ops,
               (unsigned long long)basetime_ctx.errors);
        printf("  Delete ops:  %llu, errors: %llu\n", (unsigned long long)delete_ctx.ops,
               (unsigned long long)delete_ctx.errors);
        printf("  Invalid ops: %llu, errors: %llu\n", (unsigned long long)invalid_ctx.ops,
               (unsigned long long)invalid_ctx.errors);
        race_print_err_breakdown("Replace", replace_ctx.errors, replace_ctx.err_counts);
        race_print_err_breakdown("Dump", dump_ctx.errors, dump_ctx.err_counts);
        race_print_err_breakdown("Get", get_ctx.errors, get_ctx.err_counts);
        race_print_err_breakdown("Traffic", traffic_ctx.errors, traffic_ctx.err_counts);
        race_print_err_breakdown("Basetime", basetime_ctx.errors, basetime_ctx.err_counts);
        race_print_err_breakdown("Delete", delete_ctx.errors, delete_ctx.err_counts);
        race_print_err_breakdown("Invalid", invalid_ctx.errors, invalid_ctx.err_counts);
        race_print_extack("Replace", &replace_ctx.extack);
        race_print_extack("Dump", &dump_ctx.extack);
        race_print_extack("Get", &get_ctx.extack);
        race_print_extack("Basetime", &basetime_ctx.extack);
        race_print_extack("Delete", &delete_ctx.extack);
        race_print_extack("Invalid", &invalid_ctx.extack);
    }

    if (ret != 0)
        return -ret;
    return 0;
}

int gb_race_run(const struct gb_config* cfg) {
    return gb_race_run_with_summary(cfg, NULL);
}
