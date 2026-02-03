#include "selftest_tests.h"
#include <errno.h>

int gb_selftest_priority_flags(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;
    const uint32_t gate_flags = 0x5a5aU;
    const int32_t priority = 7;
    uint32_t index_with = base_index;
    uint32_t index_without = base_index + 1;

    gb_selftest_shape_default(&shape, 1);
    gb_selftest_entry_default(&entry);

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

    ret = build_gate_newaction(msg, index_with, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, gate_flags, priority);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_get_action(sock, index_with, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup_with;
    }

    if (dump.flags != gate_flags || dump.priority != priority) {
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup_with:
    gb_selftest_cleanup_gate(sock, msg, resp, index_with);
    if (test_ret < 0)
        goto out;

    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, index_without, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_get_action(sock, index_without, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup_without;
    }

    if (dump.flags != 0 || dump.priority != -1) {
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup_without:
    gb_selftest_cleanup_gate(sock, msg, resp, index_without);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
