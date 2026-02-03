#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>

int gb_selftest_replace_without_existing(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    size_t cap;
    int ret;
    int test_ret = 0;

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
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    if (dump.num_entries != 1 || dump.entries[0].index != 0 || dump.entries[0].interval != entry.interval ||
        dump.entries[0].gate_state != entry.gate_state) {
        gb_selftest_log("REPLACE should create action: entries %u, interval %u\n", dump.num_entries,
                        dump.entries[0].interval);
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

    gb_nl_msg_reset(msg);
    ret = build_gate_delaction(msg, base_index);
    if (ret >= 0) {
        gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    }

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
