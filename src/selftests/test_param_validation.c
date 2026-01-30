#include "selftest_tests.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>

int gb_selftest_param_validation(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;
    int ret;

    ret = gb_selftest_alloc_msgs(&msg, &resp, 1024);
    if (ret < 0) {
        return ret;
    }

    /* 1. Test TCA_GATE_BASE_TIME with bad size (u32 instead of u64) */
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;

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

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u32(nlh, TCA_GATE_BASE_TIME, 0); /* WRONG SIZE: u32 instead of u64 */
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, GB_SELFTEST_DEFAULT_INTERVAL_NS);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret != -EINVAL) {
        printf("Expected -EINVAL for bad BASE_TIME size, got %d\n", ret);
        gb_selftest_cleanup_gate(sock, msg, resp, base_index);
        ret = -1;
        goto out;
    }

    /* 2. Test TCA_GATE_CYCLE_TIME with bad size (u32 instead of u64) */
    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    mnl_attr_put_str(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, base_index + 1);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);
    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u32(nlh, TCA_GATE_CYCLE_TIME, GB_SELFTEST_DEFAULT_INTERVAL_NS); /* WRONG SIZE */

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret != -EINVAL) {
        printf("Expected -EINVAL for bad CYCLE_TIME size, got %d\n", ret);
        gb_selftest_cleanup_gate(sock, msg, resp, base_index + 1);
        ret = -1;
        goto out;
    }

    /* 3. Test TCA_GATE_CYCLE_TIME zero when not derivable (no entry list) */
    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    mnl_attr_put_str(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, base_index + 2);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);
    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, 0); /* Zero and not derivable */

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret != -EINVAL) {
        printf("Expected -EINVAL for zero CYCLE_TIME when not derivable, got %d\n", ret);
        gb_selftest_cleanup_gate(sock, msg, resp, base_index + 2);
        ret = -1;
        goto out;
    }

    ret = 0;
out:
    gb_selftest_free_msgs(msg, resp);
    return ret;
}
