#include "selftest_tests.h"
#include <errno.h>

int gb_selftest_replace_without_existing(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    size_t cap;
    int ret;

    ret = gb_selftest_alloc_msgs(&msg, &resp, 1024);
    if (ret < 0) {
        return ret;
    }

    ret = build_gate_delaction(msg, base_index);
    if (ret < 0) {
        goto out;
    }

    /* Try to delete non-existing - ignore error */
    gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);

    gb_selftest_shape_default(&shape, 1);
    gb_selftest_entry_default(&entry);

    gb_nl_msg_reset(msg);
    cap = gate_msg_capacity(1, 0);
    gb_nl_msg_free(msg);
    msg = gb_nl_msg_alloc(cap);
    if (!msg) {
        ret = -ENOMEM;
        goto out;
    }

    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_REPLACE, 0, -1);
    if (ret < 0) {
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret == 0) {
        gb_nl_msg_reset(msg);
        ret = build_gate_delaction(msg, base_index);
        if (ret >= 0) {
            gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
        }
        ret = 0;
    }

out:
    gb_selftest_free_msgs(msg, resp);
    return ret;
}
