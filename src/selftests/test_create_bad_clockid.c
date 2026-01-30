#include "selftest_tests.h"
#include <errno.h>

int gb_selftest_create_bad_clockid(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    int ret;

    gb_selftest_shape_default(&shape, 1);
    shape.clockid = 999;

    gb_selftest_entry_default(&entry);

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        gb_selftest_free_msgs(msg, resp);
        return ret;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);

    gb_selftest_free_msgs(msg, resp);
    return ret;
}
