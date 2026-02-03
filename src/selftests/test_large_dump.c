#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test dumping a large number of entries to check for truncation or buffer issues */
int gb_selftest_large_dump(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry* entries;
    struct gate_dump dump;
    struct gb_dump_stats dump_stats;
    const uint32_t num_entries = 93; /* Significant but should fit in typical netlink msg */
    uint64_t cycle_time = 0;
    int ret;
    int test_ret = 0;
    bool dump_valid = false;

    entries = calloc(num_entries, sizeof(*entries));
    if (!entries)
        return -ENOMEM;

    gb_selftest_shape_default(&shape, num_entries);
    for (uint32_t i = 0; i < num_entries; i++) {
        gb_selftest_entry_default(&entries[i]);
        entries[i].index = i;
        entries[i].interval = 100000 + i;
        entries[i].gate_state = (i % 2) == 0;
        cycle_time += entries[i].interval;
    }
    shape.cycle_time = cycle_time;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(num_entries, 0));
    if (ret < 0) {
        free(entries);
        return ret;
    }

    /* Ensure index is free to avoid -EEXIST noise on reruns */
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

    ret = build_gate_newaction(msg, base_index, &shape, entries, num_entries, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    gb_selftest_log("DEBUG: large dump msg_len=%zu cap=%zu entries=%u cycle_time=%llu\n", msg->len, msg->cap,
                    num_entries, (unsigned long long)shape.cycle_time);

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("Large dump create failed: %d (%s)\n", ret, gb_nl_strerror(ret));
        test_ret = ret;
        goto cleanup;
    }

    /* Verify dump */
    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("Large dump get_action failed: %d (%s)\n", ret, gb_nl_strerror(ret));
        test_ret = ret;
        goto cleanup;
    }
    dump_valid = true;

    if (dump.num_entries != num_entries) {
        gb_selftest_log("Large dump failed: got %u entries, expected %u\n", dump.num_entries, num_entries);
        test_ret = -EINVAL;
    }
    else {
        /* Check a few entries */
        for (uint32_t i = 0; i < num_entries; i++) {
            if (dump.entries[i].index != i || dump.entries[i].interval != entries[i].interval ||
                dump.entries[i].gate_state != entries[i].gate_state) {
                gb_selftest_log("Large dump data mismatch at entry %u\n", i);
                test_ret = -EINVAL;
                break;
            }
        }
    }

cleanup:
    if (dump_valid)
        gb_gate_dump_free(&dump);

    gb_selftest_cleanup_gate(sock, msg, resp, base_index);
    gb_nl_msg_reset(msg);
    ret = build_gate_flushaction(msg);
    if (ret < 0) {
        gb_selftest_log("Gate flush build failed: %d (%s)\n", ret, gb_nl_strerror(ret));
    }
    else {
        uint32_t fcnt = UINT32_MAX;
        ret = gb_nl_send_recv_flush(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS, &fcnt);
        if (ret < 0) {
            gb_selftest_log("Gate flush failed: %d (%s)\n", ret, gb_nl_strerror(ret));
        }
        else if (fcnt == UINT32_MAX) {
            gb_selftest_log("Gate flush notification: fcnt=missing\n");
        }
        else {
            gb_selftest_log("Gate flush notification: fcnt=%u\n", fcnt);
        }
    }
    gb_nl_msg_reset(msg);
    ret = build_gate_dumpaction(msg);
    if (ret < 0) {
        gb_selftest_log("Gate dump build failed: %d (%s)\n", ret, gb_nl_strerror(ret));
    }
    else {
        memset(&dump_stats, 0, sizeof(dump_stats));
        ret = gb_nl_dump_action(sock, msg, &dump_stats, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            gb_selftest_log("Gate dump failed: %d (%s)\n", ret, gb_nl_strerror(ret));
        }
        else {
            gb_selftest_log("post-flush gate dump: actions=%u reply_msgs=%u done=%d error=%d\n",
                            dump_stats.action_count, dump_stats.reply_msgs, dump_stats.saw_done ? 1 : 0,
                            dump_stats.saw_error ? 1 : 0);
            if (dump_stats.saw_error)
                gb_selftest_log("post-flush gate dump error: %d (%s)\n", dump_stats.error_code,
                                gb_nl_strerror(dump_stats.error_code));
        }
    }

    gb_selftest_free_msgs(msg, resp);
    free(entries);
    return test_ret;
}
