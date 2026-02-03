#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define GB_TIMER_LIST_PATH "/proc/timer_list"
#define GB_TIMER_LIST_FUNC "gate_timer_func"

#define GB_TIMER_LIST_INTERVAL_NS 1000000000ull
#define GB_TIMER_LIST_OLD_DELTA_NS 2000000000ull
#define GB_TIMER_LIST_NEW_DELTA_NS 6000000000ull
#define GB_TIMER_LIST_TOL_NS 100000000ull

static int gb_clock_gettime_ns(clockid_t clkid, uint64_t* out_ns) {
    struct timespec ts;

    if (!out_ns)
        return -EINVAL;

    if (clock_gettime(clkid, &ts) != 0)
        return -errno;

    *out_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
}

static uint64_t gb_abs_diff_u64(uint64_t a, uint64_t b) {
    return a > b ? (a - b) : (b - a);
}

static int gb_timer_list_find_gate(uint64_t expected_ns,
                                   uint64_t tolerance_ns,
                                   uint64_t* found_ns,
                                   unsigned int* found_state,
                                   bool* soft_hard_mismatch) {
    FILE* fp;
    char line[4096];
    bool saw_gate = false;
    uint64_t best_diff = UINT64_MAX;
    uint64_t best_expiry = 0;
    unsigned int best_state = 0;
    bool best_soft_hard_mismatch = false;

    fp = fopen(GB_TIMER_LIST_PATH, "r");
    if (!fp)
        return -errno;

    while (fgets(line, sizeof(line), fp)) {
        unsigned int state = 0;
        unsigned long long soft = 0;
        unsigned long long hard = 0;
        char* state_pos;
        uint64_t diff;

        if (!strstr(line, GB_TIMER_LIST_FUNC))
            continue;

        saw_gate = true;

        state_pos = strstr(line, "S:");
        if (state_pos)
            sscanf(state_pos, "S:%x", &state);

        if (!fgets(line, sizeof(line), fp))
            break;

        if (sscanf(line, " # expires at %llu-%llu nsecs", &soft, &hard) != 2)
            continue;

        diff = gb_abs_diff_u64((uint64_t)hard, expected_ns);
        if (diff < best_diff) {
            best_diff = diff;
            best_expiry = (uint64_t)hard;
            best_state = state;
            best_soft_hard_mismatch = (soft != hard);
        }
    }

    fclose(fp);

    if (!saw_gate)
        return -ENOENT;

    if (found_ns)
        *found_ns = best_expiry;
    if (found_state)
        *found_state = best_state;
    if (soft_hard_mismatch)
        *soft_hard_mismatch = best_soft_hard_mismatch;

    if (best_diff > tolerance_ns)
        return -ERANGE;

    return 0;
}

int gb_selftest_timer_list_expiry(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    uint64_t now_ns = 0;
    uint64_t base_old;
    uint64_t base_new;
    uint64_t found_expiry = 0;
    unsigned int found_state = 0;
    bool soft_hard_mismatch = false;
    clockid_t clkid = CLOCK_TAI;
    int ret;
    int test_ret = 0;

    if (!sock)
        return -EINVAL;

    gb_selftest_shape_default(&shape, 1);
    gb_selftest_entry_default(&entry);

    ret = gb_clock_gettime_ns(CLOCK_TAI, &now_ns);
    if (ret < 0) {
        ret = gb_clock_gettime_ns(CLOCK_MONOTONIC, &now_ns);
        if (ret < 0)
            return ret;
        clkid = CLOCK_MONOTONIC;
    }

    shape.clockid = (uint32_t)clkid;
    shape.interval_ns = GB_TIMER_LIST_INTERVAL_NS;
    shape.cycle_time = GB_TIMER_LIST_INTERVAL_NS;
    shape.cycle_time_ext = 0;
    shape.entries = 1;

    entry.interval = (uint32_t)GB_TIMER_LIST_INTERVAL_NS;

    base_old = now_ns + GB_TIMER_LIST_OLD_DELTA_NS;
    base_new = now_ns + GB_TIMER_LIST_NEW_DELTA_NS;

    shape.base_time = base_old;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0)
        return ret;

    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    shape.base_time = base_new;
    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_REPLACE, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_timer_list_find_gate(base_new, GB_TIMER_LIST_TOL_NS, &found_expiry, &found_state, &soft_hard_mismatch);
    if (ret < 0) {
        if (ret == -ERANGE) {
            printf("timer_list expiry mismatch: expected ~%llu, got %llu\n", (unsigned long long)base_new,
                   (unsigned long long)found_expiry);
            test_ret = -EINVAL;
        }
        else if (ret == -ENOENT) {
            printf("timer_list missing gate_timer_func entry\n");
            test_ret = -EINVAL;
        }
        else if (ret == -EACCES || ret == -EPERM) {
            printf("timer_list unreadable; run as root to validate\n");
            test_ret = ret;
        }
        else {
            printf("timer_list check failed: %s\n", strerror(-ret));
            test_ret = ret;
        }
        goto cleanup;
    }

    if (soft_hard_mismatch) {
        printf("timer_list soft/hard expiry mismatch\n");
        test_ret = -EINVAL;
        goto cleanup;
    }

    if (found_state == 0) {
        printf("timer_list gate timer not enqueued (state=0x%x)\n", found_state);
        test_ret = -EINVAL;
        goto cleanup;
    }

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
