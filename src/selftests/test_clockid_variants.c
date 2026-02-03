#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>

int gb_selftest_clockid_variants(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;
    uint32_t valid_clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_TAI};
    size_t i;

    gb_selftest_shape_default(&shape, 1);
    gb_selftest_entry_default(&entry);

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

    for (i = 0; i < sizeof(valid_clocks) / sizeof(valid_clocks[0]); i++) {
        uint32_t clockid = valid_clocks[i];

        gb_selftest_cleanup_gate(sock, msg, resp, base_index);

        shape.clockid = clockid;
        gb_nl_msg_reset(msg);
        ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
        if (ret < 0) {
            test_ret = ret;
            goto out;
        }

        ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            gb_selftest_log("Failed to create with clockid %u: %d\n", clockid, ret);
            test_ret = ret;
            goto cleanup;
        }

        ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            test_ret = ret;
            goto cleanup;
        }

        if (dump.clockid != clockid) {
            gb_selftest_log("Clock ID mismatch: expected %u, got %u\n", clockid, dump.clockid);
            test_ret = -EINVAL;
            gb_gate_dump_free(&dump);
            goto cleanup;
        }
        gb_gate_dump_free(&dump);
    }

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
