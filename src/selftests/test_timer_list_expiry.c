#include "selftest_tests.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define GB_TIMER_LIST_PATH "/proc/timer_list"
#define GB_TIMER_LIST_FUNC "gate_timer_func"

#define GB_TIMER_LIST_INTERVAL_NS 1000000000ull
#define GB_TIMER_LIST_OLD_DELTA_NS 2000000000ull
#define GB_TIMER_LIST_NEW_DELTA_NS 6000000000ull
#define GB_TIMER_LIST_TOL_NS 100000000ull
#define GB_TIMER_LIST_RETRIES 5
#define GB_TIMER_LIST_RETRY_NS 20000000ull
#define GB_TIMER_LIST_BASELINE_ENTRIES 64
#define GB_TIMER_LIST_BASELINE_PATTERN_LEN 8

static const uint32_t gb_baseline_intervals[GB_TIMER_LIST_BASELINE_PATTERN_LEN] = {
    250000, 500000, 750000, 1000000, 1250000, 1500000, 1750000, 2000000,
};

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

static uint64_t gb_fill_baseline_entries(struct gate_entry* entries, uint32_t count) {
    uint64_t cycle = 0;

    if (!entries || count == 0)
        return 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t interval = gb_baseline_intervals[i % GB_TIMER_LIST_BASELINE_PATTERN_LEN];

        entries[i].index = i;
        entries[i].gate_state = (i % 2) == 0;
        entries[i].interval = interval;
        entries[i].ipv = -1;
        entries[i].maxoctets = -1;
        cycle += interval;
    }

    return cycle;
}

static int gb_timer_list_find_gate(uint64_t expected_ns,
                                   uint64_t tolerance_ns,
                                   uint64_t* found_soft_ns,
                                   uint64_t* found_hard_ns,
                                   unsigned int* found_state,
                                   bool* soft_hard_mismatch) {
    FILE* fp;
    char line[4096];
    bool saw_gate = false;
    uint64_t best_diff = UINT64_MAX;
    uint64_t best_expiry = 0;
    uint64_t best_soft = 0;
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
            best_soft = (uint64_t)soft;
            best_state = state;
            best_soft_hard_mismatch = (soft != hard);
        }
    }

    fclose(fp);

    if (!saw_gate)
        return -ENOENT;

    if (found_soft_ns)
        *found_soft_ns = best_soft;
    if (found_hard_ns)
        *found_hard_ns = best_expiry;
    if (found_state)
        *found_state = best_state;
    if (soft_hard_mismatch)
        *soft_hard_mismatch = best_soft_hard_mismatch;

    if (best_diff > tolerance_ns)
        return -ERANGE;

    return 0;
}

static int gb_timer_list_expect(const char* label,
                                uint64_t expected_ns,
                                uint64_t tolerance_ns,
                                uint64_t max_allowed_ns) {
    struct timespec req = {0};
    uint64_t found_expiry = 0;
    uint64_t found_soft = 0;
    unsigned int found_state = 0;
    bool soft_hard_mismatch = false;
    int ret = -ERANGE;
    uint64_t diff;

    req.tv_nsec = (long)GB_TIMER_LIST_RETRY_NS;

    for (int i = 0; i < GB_TIMER_LIST_RETRIES; i++) {
        ret = gb_timer_list_find_gate(expected_ns, tolerance_ns, &found_soft, &found_expiry, &found_state,
                                      &soft_hard_mismatch);
        if (ret == 0 || ret == -EACCES || ret == -EPERM)
            break;
        nanosleep(&req, NULL);
    }

    if (ret < 0) {
        if (ret == -ERANGE) {
            printf("%s: expiry mismatch (expected ~%llu, got %llu)\n", label, (unsigned long long)expected_ns,
                   (unsigned long long)found_expiry);
            return -EINVAL;
        }
        if (ret == -ENOENT) {
            printf("%s: missing gate_timer_func entry\n", label);
            return -EINVAL;
        }
        if (ret == -EACCES || ret == -EPERM) {
            printf("%s: timer_list unreadable; run as root to validate\n", label);
            return ret;
        }
        printf("%s: timer_list check failed: %s\n", label, strerror(-ret));
        return ret;
    }

    diff = gb_abs_diff_u64(found_expiry, expected_ns);
    printf("%s: expected=%llu +/- %llu, found=%llu (soft=%llu) diff=%llu state=0x%x\n", label,
           (unsigned long long)expected_ns, (unsigned long long)tolerance_ns, (unsigned long long)found_expiry,
           (unsigned long long)found_soft, (unsigned long long)diff, found_state);

    if (soft_hard_mismatch) {
        printf("%s: soft/hard expiry mismatch\n", label);
        return -EINVAL;
    }

    if (found_state == 0) {
        printf("%s: gate timer not enqueued (state=0x%x)\n", label, found_state);
        return -EINVAL;
    }

    if (max_allowed_ns != 0 && found_expiry > max_allowed_ns + tolerance_ns) {
        printf("%s: expiry wrapped (got %llu > %llu)\n", label, (unsigned long long)found_expiry,
               (unsigned long long)max_allowed_ns);
        return -EINVAL;
    }

    return 0;
}

static int gb_calc_expected_start(uint64_t now_ns, uint64_t base_ns, uint64_t cycle_ns, uint64_t* out_ns) {
    __int128 start;
    uint64_t diff;
    uint64_t n;

    if (!out_ns || cycle_ns == 0)
        return -EINVAL;

    if (base_ns > (uint64_t)INT64_MAX || cycle_ns > (uint64_t)INT64_MAX)
        return -ERANGE;

    if (base_ns > now_ns) {
        *out_ns = base_ns;
        return 0;
    }

    diff = now_ns - base_ns;
    n = diff / cycle_ns;
    start = (__int128)base_ns + ((__int128)(n + 1) * (__int128)cycle_ns);
    if (start > INT64_MAX)
        return -ERANGE;

    *out_ns = (uint64_t)start;
    return 0;
}

int gb_selftest_timer_list_expiry(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_entry baseline_entries[GB_TIMER_LIST_BASELINE_ENTRIES];
    uint64_t now_ns = 0;
    uint64_t base_old;
    uint64_t base_new;
    uint64_t expected_ns = 0;
    uint64_t base_epoch = 0;
    uint64_t base_past = 0;
    uint64_t base_max = 0;
    uint64_t large_cycle = 0;
    uint64_t baseline_cycle = 0;
    clockid_t clkid = CLOCK_TAI;
    int ret;
    int test_ret = 0;

    if (!sock)
        return -EINVAL;

    gb_selftest_shape_default(&shape, 1);
    gb_selftest_entry_default(&entry);
    baseline_cycle = gb_fill_baseline_entries(baseline_entries, GB_TIMER_LIST_BASELINE_ENTRIES);

    ret = gb_clock_gettime_ns(CLOCK_TAI, &now_ns);
    if (ret < 0) {
        ret = gb_clock_gettime_ns(CLOCK_MONOTONIC, &now_ns);
        if (ret < 0)
            return ret;
        clkid = CLOCK_MONOTONIC;
    }

    shape.clockid = (uint32_t)clkid;
    shape.interval_ns = gb_baseline_intervals[0];
    shape.cycle_time = baseline_cycle;
    shape.cycle_time_ext = 0;
    shape.entries = GB_TIMER_LIST_BASELINE_ENTRIES;
    shape.base_time = now_ns + baseline_cycle;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(GB_TIMER_LIST_BASELINE_ENTRIES, 0));
    if (ret < 0)
        return ret;

    ret = build_gate_newaction(msg, base_index, &shape, baseline_entries, GB_TIMER_LIST_BASELINE_ENTRIES,
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

    printf("baseline-schedule: now=%llu base=%llu cycle=%llu entries=%u pattern=%u,%u,%u,%u,%u,%u,%u,%u\n",
           (unsigned long long)now_ns, (unsigned long long)shape.base_time, (unsigned long long)shape.cycle_time,
           GB_TIMER_LIST_BASELINE_ENTRIES, gb_baseline_intervals[0], gb_baseline_intervals[1], gb_baseline_intervals[2],
           gb_baseline_intervals[3], gb_baseline_intervals[4], gb_baseline_intervals[5], gb_baseline_intervals[6],
           gb_baseline_intervals[7]);
    test_ret = gb_timer_list_expect("baseline-schedule", shape.base_time, GB_TIMER_LIST_TOL_NS, 0);
    if (test_ret < 0)
        goto cleanup;

    ret = gb_clock_gettime_ns(clkid, &now_ns);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    shape.interval_ns = GB_TIMER_LIST_INTERVAL_NS;
    shape.cycle_time = GB_TIMER_LIST_INTERVAL_NS;
    shape.cycle_time_ext = 0;
    shape.entries = 1;
    entry.interval = (uint32_t)GB_TIMER_LIST_INTERVAL_NS;

    base_old = now_ns + GB_TIMER_LIST_OLD_DELTA_NS;
    base_new = now_ns + GB_TIMER_LIST_NEW_DELTA_NS;
    shape.base_time = base_old;

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

    printf("replace-old: base=%llu cycle=%llu interval=%u\n", (unsigned long long)base_old,
           (unsigned long long)shape.cycle_time, entry.interval);
    test_ret = gb_timer_list_expect("replace-old", base_old, GB_TIMER_LIST_TOL_NS, 0);
    if (test_ret < 0)
        goto cleanup;

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

    printf("replace-forward: base_old=%llu base_new=%llu cycle=%llu interval=%u\n", (unsigned long long)base_old,
           (unsigned long long)base_new, (unsigned long long)shape.cycle_time, entry.interval);
    test_ret = gb_timer_list_expect("replace-forward", base_new, GB_TIMER_LIST_TOL_NS, 0);
    if (test_ret < 0)
        goto cleanup;

    ret = gb_clock_gettime_ns(clkid, &now_ns);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (now_ns > 1500000000ull)
        base_past = now_ns - 1500000000ull;
    else
        base_past = 0;

    shape.base_time = base_past;
    shape.cycle_time = GB_TIMER_LIST_INTERVAL_NS;
    shape.interval_ns = GB_TIMER_LIST_INTERVAL_NS;
    entry.interval = (uint32_t)GB_TIMER_LIST_INTERVAL_NS;

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
    ret = gb_calc_expected_start(now_ns, base_past, shape.cycle_time, &expected_ns);
    if (ret < 0) {
        printf("past-base: expected start overflow\n");
        test_ret = -EINVAL;
        goto cleanup;
    }
    printf("past-base: now=%llu base=%llu cycle=%llu interval=%u\n", (unsigned long long)now_ns,
           (unsigned long long)base_past, (unsigned long long)shape.cycle_time, entry.interval);
    test_ret = gb_timer_list_expect("past-base", expected_ns, GB_TIMER_LIST_TOL_NS * 2, 0);
    if (test_ret < 0)
        goto cleanup;

    ret = gb_clock_gettime_ns(clkid, &now_ns);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    {
        uint64_t offset = (GB_TIMER_LIST_INTERVAL_NS * 5ull) + (GB_TIMER_LIST_INTERVAL_NS / 2ull);
        if (now_ns > offset)
            base_epoch = now_ns - offset;
        else
            base_epoch = 0;
    }
    shape.base_time = base_epoch;
    shape.cycle_time = GB_TIMER_LIST_INTERVAL_NS;
    shape.interval_ns = GB_TIMER_LIST_INTERVAL_NS;
    entry.interval = (uint32_t)GB_TIMER_LIST_INTERVAL_NS;

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
    ret = gb_calc_expected_start(now_ns, base_epoch, shape.cycle_time, &expected_ns);
    if (ret < 0) {
        printf("mid-cycle-past: expected start overflow\n");
        test_ret = -EINVAL;
        goto cleanup;
    }
    printf("mid-cycle-past: now=%llu base=%llu cycle=%llu interval=%u\n", (unsigned long long)now_ns,
           (unsigned long long)base_epoch, (unsigned long long)shape.cycle_time, entry.interval);
    test_ret = gb_timer_list_expect("mid-cycle-past", expected_ns, GB_TIMER_LIST_TOL_NS * 2, 0);
    if (test_ret < 0)
        goto cleanup;

    base_max = (uint64_t)INT64_MAX - 2000000000ull;
    if (base_max > now_ns) {
        shape.base_time = base_max;
        shape.cycle_time = GB_TIMER_LIST_INTERVAL_NS;
        shape.interval_ns = GB_TIMER_LIST_INTERVAL_NS;
        entry.interval = (uint32_t)GB_TIMER_LIST_INTERVAL_NS;

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
        printf("near-max-future: base=%llu cycle=%llu interval=%u\n", (unsigned long long)base_max,
               (unsigned long long)shape.cycle_time, entry.interval);
        test_ret = gb_timer_list_expect("near-max-future", base_max, GB_TIMER_LIST_TOL_NS, (uint64_t)INT64_MAX);
        if (test_ret < 0)
            goto cleanup;
    }

    ret = gb_clock_gettime_ns(clkid, &now_ns);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    large_cycle = (uint64_t)INT64_MAX / 4ull;
    if (now_ns > 1000000000ull)
        base_past = now_ns - 1000000000ull;
    else
        base_past = 0;

    shape.base_time = base_past;
    shape.cycle_time = large_cycle;
    shape.interval_ns = GB_TIMER_LIST_INTERVAL_NS;
    entry.interval = (uint32_t)GB_TIMER_LIST_INTERVAL_NS;

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
    ret = gb_calc_expected_start(now_ns, base_past, large_cycle, &expected_ns);
    if (ret < 0) {
        printf("large-cycle: expected start overflow\n");
        test_ret = -EINVAL;
        goto cleanup;
    }
    printf("large-cycle: now=%llu base=%llu cycle=%llu interval=%u\n", (unsigned long long)now_ns,
           (unsigned long long)base_past, (unsigned long long)large_cycle, entry.interval);
    test_ret = gb_timer_list_expect("large-cycle", expected_ns, GB_TIMER_LIST_TOL_NS, (uint64_t)INT64_MAX);
    if (test_ret < 0)
        goto cleanup;

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
