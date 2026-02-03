#include "selftest_tests.h"
#include <errno.h>

int gb_selftest_cycle_time_supplied(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entries[2];
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 2);
    shape.cycle_time = 5000000; /* 5 ms, does not match entries sum. */

    gb_selftest_entry_default(&entries[0]);
    entries[0].index = 0;
    entries[0].interval = 1000000;
    entries[0].gate_state = true;

    gb_selftest_entry_default(&entries[1]);
    entries[1].index = 1;
    entries[1].interval = 2000000;
    entries[1].gate_state = false;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(2, 0));
    if (ret < 0) {
        return ret;
    }

    ret = build_gate_newaction(msg, base_index, &shape, entries, 2, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
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

    if (dump.cycle_time != shape.cycle_time || dump.num_entries != 2) {
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
