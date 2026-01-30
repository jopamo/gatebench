#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>

int gb_selftest_base_time_update(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 1);
    shape.base_time = 1000000;

    gb_selftest_entry_default(&entry);

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

    /* 1. Create gate with base_time 1ms */
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

    /* 2. Replace with base_time 2ms and NO entries */
    shape.base_time = 2000000;
    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &shape, NULL, 0, NLM_F_REPLACE, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    /* 3. Verify dump: base_time should be 2ms, entry should be preserved */
    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.base_time != 2000000 || dump.num_entries != 1) {
        printf("Base time update failed: base %lu, entries %u\n", dump.base_time, dump.num_entries);
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
