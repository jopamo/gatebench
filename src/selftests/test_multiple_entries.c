#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>

int gb_selftest_multiple_entries(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entries[3];
    struct gate_dump dump;
    int ret;
    int test_ret = 0;
    uint32_t i;

    gb_selftest_shape_default(&shape, 3);
    shape.cycle_time = 3000000;

    entries[0].gate_state = true;
    entries[0].index = 0;
    entries[0].interval = 1000000;
    entries[0].ipv = 4;
    entries[0].maxoctets = 100;

    entries[1].gate_state = false;
    entries[1].index = 1;
    entries[1].interval = 1000000;
    entries[1].ipv = 6;
    entries[1].maxoctets = 200;

    entries[2].gate_state = true;
    entries[2].index = 2;
    entries[2].interval = 1000000;
    entries[2].ipv = -1;
    entries[2].maxoctets = -1;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(3, 0));
    if (ret < 0) {
        return ret;
    }

    ret = build_gate_newaction(msg, base_index, &shape, entries, 3, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.num_entries != 3) {
        gb_selftest_log("Multiple entries failed: expected 3, got %u\n", dump.num_entries);
        test_ret = -EINVAL;
        gb_gate_dump_free(&dump);
        goto cleanup;
    }

    for (i = 0; i < 3; i++) {
        if (dump.entries[i].index != i || dump.entries[i].gate_state != entries[i].gate_state ||
            dump.entries[i].interval != entries[i].interval || dump.entries[i].ipv != entries[i].ipv ||
            dump.entries[i].maxoctets != entries[i].maxoctets) {
            gb_selftest_log("Entry %u mismatch\n", i);
            test_ret = -EINVAL;
            break;
        }
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
