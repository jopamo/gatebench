#include "selftest_tests.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define GB_RCU_SNAPSHOT_ENTRIES 3u
#define GB_RCU_SNAPSHOT_REPLACE_ITERS 320u
#define GB_RCU_SNAPSHOT_DUMP_ITERS 960u
#define GB_RCU_SNAPSHOT_BASE_DELAY_NS 50000000ULL

struct gb_replace_worker_ctx {
    uint32_t index;
    struct gate_entry entries_a[GB_RCU_SNAPSHOT_ENTRIES];
    struct gate_entry entries_b[GB_RCU_SNAPSHOT_ENTRIES];
    uint64_t cycle_a;
    uint64_t cycle_b;
    int err;
};

struct gb_dump_worker_ctx {
    uint32_t index;
    struct gate_entry entries_a[GB_RCU_SNAPSHOT_ENTRIES];
    struct gate_entry entries_b[GB_RCU_SNAPSHOT_ENTRIES];
    int err;
};

static uint64_t sum_intervals(const struct gate_entry* entries, uint32_t n) {
    uint64_t total = 0;

    for (uint32_t i = 0; i < n; i++)
        total += entries[i].interval;

    return total;
}

static int64_t now_ns(clockid_t clockid) {
    struct timespec ts;

    if (clock_gettime(clockid, &ts) < 0)
        return (int64_t)(-errno);

    return ((int64_t)ts.tv_sec * 1000000000LL) + (int64_t)ts.tv_nsec;
}

static int build_gate_replace_sparse(struct gb_nl_msg* msg,
                                     uint32_t index,
                                     bool add_clockid,
                                     int32_t clockid,
                                     bool add_base_time,
                                     uint64_t base_time,
                                     bool add_cycle_time,
                                     uint64_t cycle_time) {
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;

    if (!msg || !msg->buf)
        return -EINVAL;

    gb_nl_msg_reset(msg);

    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_REPLACE;
    nlh->nlmsg_seq = 0;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    mnl_attr_put_strz(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, index);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    if (add_clockid)
        mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, (uint32_t)clockid);
    if (add_base_time)
        mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, base_time);
    if (add_cycle_time)
        mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, cycle_time);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);
    msg->len = nlh->nlmsg_len;

    return 0;
}

static bool entry_equal(const struct gate_entry* a, const struct gate_entry* b) {
    return a->gate_state == b->gate_state && a->interval == b->interval && a->ipv == b->ipv &&
           a->maxoctets == b->maxoctets;
}

static bool dump_matches_entries(const struct gate_dump* dump, const struct gate_entry* entries, uint32_t count) {
    if (!dump || !entries)
        return false;
    if (dump->num_entries != count)
        return false;

    for (uint32_t i = 0; i < count; i++) {
        if (!entry_equal(&dump->entries[i], &entries[i]))
            return false;
    }

    return true;
}

static int prepare_shape(struct gate_shape* shape, clockid_t clockid, uint64_t cycle_time) {
    int64_t now;

    if (!shape)
        return -EINVAL;

    now = now_ns(clockid);
    if (now < 0)
        return (int)now;

    gb_selftest_shape_default(shape, GB_RCU_SNAPSHOT_ENTRIES);
    shape->clockid = (uint32_t)clockid;
    shape->base_time = (uint64_t)(now + (int64_t)GB_RCU_SNAPSHOT_BASE_DELAY_NS);
    shape->cycle_time = cycle_time;
    return 0;
}

static void* replace_worker(void* arg) {
    struct gb_replace_worker_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    size_t cap;
    int ret;

    if (!ctx) {
        return NULL;
    }

    cap = gate_msg_capacity(GB_RCU_SNAPSHOT_ENTRIES, 0);
    ret = gb_nl_open(&sock);
    if (ret < 0) {
        ctx->err = ret;
        return NULL;
    }

    ret = gb_selftest_alloc_msgs(&msg, &resp, cap);
    if (ret < 0) {
        ctx->err = ret;
        gb_nl_close(sock);
        return NULL;
    }

    for (uint32_t i = 0; i < GB_RCU_SNAPSHOT_REPLACE_ITERS; i++) {
        uint32_t phase = i & 3u;

        if (phase == 0u) {
            ret = prepare_shape(&shape, CLOCK_TAI, ctx->cycle_a);
            if (ret < 0)
                break;
            ret = build_gate_newaction(msg, ctx->index, &shape, ctx->entries_a, GB_RCU_SNAPSHOT_ENTRIES, NLM_F_REPLACE,
                                       0, -1);
        }
        else if (phase == 1u) {
            int64_t now = now_ns(CLOCK_TAI);

            if (now < 0) {
                ret = (int)now;
                break;
            }

            ret = build_gate_replace_sparse(msg, ctx->index, false, CLOCK_TAI, true,
                                            (uint64_t)(now + (int64_t)GB_RCU_SNAPSHOT_BASE_DELAY_NS), false, 0);
        }
        else if (phase == 2u) {
            ret = prepare_shape(&shape, CLOCK_MONOTONIC, ctx->cycle_b);
            if (ret < 0)
                break;
            ret = build_gate_newaction(msg, ctx->index, &shape, ctx->entries_b, GB_RCU_SNAPSHOT_ENTRIES, NLM_F_REPLACE,
                                       0, -1);
        }
        else {
            int64_t now = now_ns(CLOCK_MONOTONIC);

            if (now < 0) {
                ret = (int)now;
                break;
            }

            ret = build_gate_replace_sparse(msg, ctx->index, true, CLOCK_MONOTONIC, true,
                                            (uint64_t)(now + (int64_t)GB_RCU_SNAPSHOT_BASE_DELAY_NS), true,
                                            ctx->cycle_b + 1000000ULL);
        }

        if (ret < 0)
            break;

        ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0)
            break;
    }

    if (ret < 0)
        ctx->err = ret;

    gb_selftest_free_msgs(msg, resp);
    gb_nl_close(sock);
    return NULL;
}

static void* dump_worker(void* arg) {
    struct gb_dump_worker_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gate_dump dump;
    int ret;

    if (!ctx) {
        return NULL;
    }

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        ctx->err = ret;
        return NULL;
    }

    for (uint32_t i = 0; i < GB_RCU_SNAPSHOT_DUMP_ITERS; i++) {
        memset(&dump, 0, sizeof(dump));
        ret = gb_nl_get_action(sock, ctx->index, &dump, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0)
            break;

        if (dump.num_entries != GB_RCU_SNAPSHOT_ENTRIES || dump.cycle_time == 0) {
            ret = -EINVAL;
            gb_gate_dump_free(&dump);
            break;
        }

        if (!dump_matches_entries(&dump, ctx->entries_a, GB_RCU_SNAPSHOT_ENTRIES) &&
            !dump_matches_entries(&dump, ctx->entries_b, GB_RCU_SNAPSHOT_ENTRIES)) {
            ret = -EINVAL;
            gb_gate_dump_free(&dump);
            break;
        }

        gb_gate_dump_free(&dump);
    }

    if (ret < 0)
        ctx->err = ret;

    gb_nl_close(sock);
    return NULL;
}

int gb_selftest_replace_rcu_snapshot(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gb_replace_worker_ctx replace_ctx;
    struct gb_dump_worker_ctx dump_ctx;
    struct gate_shape shape;
    struct gate_dump dump;
    pthread_t replace_thread;
    pthread_t dump_thread;
    bool replace_started = false;
    bool dump_started = false;
    size_t cap;
    int ret;
    int test_ret = 0;

    if (!sock)
        return -EINVAL;

    memset(&replace_ctx, 0, sizeof(replace_ctx));
    memset(&dump_ctx, 0, sizeof(dump_ctx));

    gb_selftest_entry_default(&replace_ctx.entries_a[0]);
    replace_ctx.entries_a[0].gate_state = true;
    replace_ctx.entries_a[0].interval = 1000000u;
    replace_ctx.entries_a[0].ipv = 2;
    replace_ctx.entries_a[0].maxoctets = 512;

    gb_selftest_entry_default(&replace_ctx.entries_a[1]);
    replace_ctx.entries_a[1].gate_state = false;
    replace_ctx.entries_a[1].interval = 2000000u;
    replace_ctx.entries_a[1].ipv = -1;
    replace_ctx.entries_a[1].maxoctets = -1;

    gb_selftest_entry_default(&replace_ctx.entries_a[2]);
    replace_ctx.entries_a[2].gate_state = true;
    replace_ctx.entries_a[2].interval = 3000000u;
    replace_ctx.entries_a[2].ipv = 5;
    replace_ctx.entries_a[2].maxoctets = 4096;

    gb_selftest_entry_default(&replace_ctx.entries_b[0]);
    replace_ctx.entries_b[0].gate_state = false;
    replace_ctx.entries_b[0].interval = 1500000u;
    replace_ctx.entries_b[0].ipv = 4;
    replace_ctx.entries_b[0].maxoctets = 1536;

    gb_selftest_entry_default(&replace_ctx.entries_b[1]);
    replace_ctx.entries_b[1].gate_state = true;
    replace_ctx.entries_b[1].interval = 2500000u;
    replace_ctx.entries_b[1].ipv = -1;
    replace_ctx.entries_b[1].maxoctets = -1;

    gb_selftest_entry_default(&replace_ctx.entries_b[2]);
    replace_ctx.entries_b[2].gate_state = false;
    replace_ctx.entries_b[2].interval = 3500000u;
    replace_ctx.entries_b[2].ipv = 6;
    replace_ctx.entries_b[2].maxoctets = 8192;

    replace_ctx.index = base_index;
    replace_ctx.cycle_a = sum_intervals(replace_ctx.entries_a, GB_RCU_SNAPSHOT_ENTRIES);
    replace_ctx.cycle_b = sum_intervals(replace_ctx.entries_b, GB_RCU_SNAPSHOT_ENTRIES);

    dump_ctx.index = base_index;
    memcpy(dump_ctx.entries_a, replace_ctx.entries_a, sizeof(dump_ctx.entries_a));
    memcpy(dump_ctx.entries_b, replace_ctx.entries_b, sizeof(dump_ctx.entries_b));

    cap = gate_msg_capacity(GB_RCU_SNAPSHOT_ENTRIES, 0);
    ret = gb_selftest_alloc_msgs(&msg, &resp, cap);
    if (ret < 0)
        return ret;

    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

    ret = prepare_shape(&shape, CLOCK_TAI, replace_ctx.cycle_a);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = build_gate_newaction(msg, base_index, &shape, replace_ctx.entries_a, GB_RCU_SNAPSHOT_ENTRIES,
                               NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = pthread_create(&replace_thread, NULL, replace_worker, &replace_ctx);
    if (ret != 0) {
        test_ret = -ret;
        goto cleanup;
    }
    replace_started = true;

    ret = pthread_create(&dump_thread, NULL, dump_worker, &dump_ctx);
    if (ret != 0) {
        test_ret = -ret;
        goto cleanup;
    }
    dump_started = true;

cleanup:
    if (replace_started)
        pthread_join(replace_thread, NULL);
    if (dump_started)
        pthread_join(dump_thread, NULL);

    if (test_ret == 0 && replace_ctx.err < 0) {
        gb_selftest_log("replace worker failed: %d\n", replace_ctx.err);
        test_ret = replace_ctx.err;
    }
    if (test_ret == 0 && dump_ctx.err < 0) {
        gb_selftest_log("dump worker failed: %d\n", dump_ctx.err);
        test_ret = dump_ctx.err;
    }

    if (test_ret == 0) {
        memset(&dump, 0, sizeof(dump));
        ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            test_ret = ret;
        }
        else {
            bool matches_a = dump_matches_entries(&dump, replace_ctx.entries_a, GB_RCU_SNAPSHOT_ENTRIES);
            bool matches_b = dump_matches_entries(&dump, replace_ctx.entries_b, GB_RCU_SNAPSHOT_ENTRIES);

            if (dump.num_entries != GB_RCU_SNAPSHOT_ENTRIES || (!matches_a && !matches_b))
                test_ret = -EINVAL;
            gb_gate_dump_free(&dump);
        }
    }

    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
