#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int gb_selftest_replace_invalid(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 1);
    shape.base_time = 1234567;
    gb_selftest_entry_default(&entry);
    entry.interval = 1000000;

    ret = gb_selftest_alloc_msgs(&msg, &resp, 2048);
    if (ret < 0) {
        return ret;
    }

    /* 1. Create a valid gate action */
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

    /* 2. Attempt REPLACE with invalid attribute (bad size for clockid) */
    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_REPLACE;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    mnl_attr_put_str(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, base_index);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = base_index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    /* Invalid clockid size (u64 instead of u32) - should cause -EINVAL */
    mnl_attr_put_u64(nlh, TCA_GATE_CLOCKID, CLOCK_MONOTONIC);
    /* Change base_time to something else; if REPLACE succeeded, this would be updated */
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 9999999);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret != -EINVAL) {
        gb_selftest_log("Expected -EINVAL for invalid REPLACE, got %d\n", ret);
        test_ret = -1;
        goto cleanup;
    }

    /* 3. Verify that the original action is still intact */
    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.base_time != 1234567) {
        gb_selftest_log("REPLACE failure corrupted state: base_time %lu (expected 1234567)\n", dump.base_time);
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
