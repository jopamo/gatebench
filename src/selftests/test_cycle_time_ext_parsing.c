#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>

int gb_selftest_cycle_time_ext_parsing(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 1);
    shape.cycle_time_ext = 500000;

    gb_selftest_entry_default(&entry);

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

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

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.cycle_time_ext != shape.cycle_time_ext) {
        gb_selftest_log("Cycle time extension parsing failed: expected %lu, got %lu\n", shape.cycle_time_ext,
                        dump.cycle_time_ext);
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
