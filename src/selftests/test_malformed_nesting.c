#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int gb_selftest_malformed_nesting(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts, *entry_list;
    struct nlattr* entry_nest;
    struct tc_gate gate_params;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    ret = gb_selftest_alloc_msgs(&msg, &resp, 1024);
    if (ret < 0) {
        return ret;
    }

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
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, GB_SELFTEST_DEFAULT_INTERVAL_NS);

    /* Malformed entry list: non-nested attribute plus one valid entry */
    entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
    mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, GB_SELFTEST_DEFAULT_INTERVAL_NS);
    entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);
    mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);
    mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, GB_SELFTEST_DEFAULT_INTERVAL_NS);
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

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.num_entries != 1) {
        printf("Malformed nesting should skip invalid entries: got %u entries\n", dump.num_entries);
        test_ret = -EINVAL;
    }
    else if (dump.entries[0].index != 0 || dump.entries[0].interval != GB_SELFTEST_DEFAULT_INTERVAL_NS ||
             !dump.entries[0].gate_state) {
        printf("Malformed nesting preserved wrong entry contents\n");
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
