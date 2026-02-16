#include "selftest_tests.h"
#include <errno.h>

int gb_selftest_replace_persistence(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 1);
    gb_selftest_entry_default(&entry);

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

    /* 1. Create gate with flags=1, priority=10 */
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 1, 10);
    if (ret < 0) {
        gb_selftest_log("step create/build failed: %d\n", ret);
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("step create/send failed: %d\n", ret);
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("step create/get failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }
    if (dump.flags != 1 || dump.priority != 10) {
        test_ret = -EINVAL;
        gb_gate_dump_free(&dump);
        goto cleanup;
    }
    gb_gate_dump_free(&dump);

    /* 2. Replace gate with flags=2, priority=20 */
    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_REPLACE, 2, 20);
    if (ret < 0) {
        gb_selftest_log("step replace/build failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("step replace/send failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("step replace/get failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }
    if (dump.flags != 2 || dump.priority != 20) {
        gb_selftest_log("step replace/verify mismatch: flags=%u prio=%d\n", dump.flags, dump.priority);
        test_ret = -EINVAL;
        gb_gate_dump_free(&dump);
        goto cleanup;
    }
    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
