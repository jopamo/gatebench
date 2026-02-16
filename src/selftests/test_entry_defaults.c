#include "selftest_tests.h"
#include <errno.h>
#include <string.h>

int gb_selftest_entry_defaults(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts, *entry_list, *entry_nest;
    struct gate_shape shape;
    struct tc_gate gate_params;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;
    uint32_t index_missing_interval = base_index;
    uint32_t index_missing_defaults = base_index + 1;

    gb_selftest_shape_default(&shape, 1);

    ret = gb_selftest_alloc_msgs(&msg, &resp, 1024);
    if (ret < 0) {
        return ret;
    }

    /* Case 1: missing interval attribute must fail with -EINVAL. */
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nlh->nlmsg_seq = 0;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    mnl_attr_put_strz(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, index_missing_interval);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index_missing_interval;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, shape.clockid);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, shape.base_time);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, shape.cycle_time);

    entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
    entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);
    mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);
    /* Deliberately omit TCA_GATE_ENTRY_INTERVAL. */
    mnl_attr_nest_end(nlh, entry_nest);
    mnl_attr_nest_end(nlh, entry_list);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret != -EINVAL) {
        test_ret = ret == 0 ? -EINVAL : ret;
        gb_selftest_cleanup_gate(sock, msg, resp, index_missing_interval);
        goto out;
    }

    gb_selftest_cleanup_gate(sock, msg, resp, index_missing_interval);

    /* Case 2: missing IPV/MAX_OCTETS must default to -1. */
    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nlh->nlmsg_seq = 0;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    mnl_attr_put_strz(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, index_missing_defaults);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index_missing_defaults;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, shape.clockid);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, shape.base_time);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, shape.cycle_time);

    entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
    entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);
    mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);
    mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, GB_SELFTEST_DEFAULT_INTERVAL_NS);
    /* Deliberately omit IPV and MAX_OCTETS. */
    mnl_attr_nest_end(nlh, entry_nest);
    mnl_attr_nest_end(nlh, entry_list);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_get_action(sock, index_missing_defaults, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup_defaults;
    }

    if (dump.num_entries != 1 || dump.entries[0].ipv != -1 || dump.entries[0].maxoctets != -1) {
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup_defaults:
    gb_selftest_cleanup_gate(sock, msg, resp, index_missing_defaults);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
