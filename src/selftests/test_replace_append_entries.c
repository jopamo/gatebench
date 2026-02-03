#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>

int gb_selftest_replace_append_entries(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry1;
    struct gate_entry entry2;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 1);

    gb_selftest_entry_default(&entry1);
    entry1.interval = 1000000;
    entry1.gate_state = true;

    gb_selftest_entry_default(&entry2);
    entry2.interval = 2000000;
    entry2.gate_state = false;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

    ret = build_gate_newaction(msg, base_index, &shape, &entry1, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &shape, &entry2, 1, NLM_F_REPLACE, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.num_entries != 2) {
        gb_selftest_log("REPLACE append failed: expected 2 entries, got %u\n", dump.num_entries);
        test_ret = -EINVAL;
    }
    else if (dump.entries[0].interval != entry1.interval || dump.entries[0].gate_state != entry1.gate_state ||
             dump.entries[1].interval != entry2.interval || dump.entries[1].gate_state != entry2.gate_state) {
        gb_selftest_log("REPLACE append failed: entry order or contents mismatch\n");
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
