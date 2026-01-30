#include "selftest_tests.h"
#include <errno.h>

int gb_selftest_duplicate_create(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    size_t cap;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 1);
    gb_selftest_entry_default(&entry);

    cap = gate_msg_capacity(1, 0);
    ret = gb_selftest_alloc_msgs(&msg, &resp, cap);
    if (ret < 0) {
        return ret;
    }

    /* First create */
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0 && ret != -EEXIST) {
        test_ret = ret;
        goto cleanup;
    }

    /* Try to create again - should fail with -EEXIST */
    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    test_ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
