#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <linux/netlink.h>

static int send_invalid_entry(struct gb_nl_sock* sock,
                              struct gb_nl_msg* msg,
                              struct gb_nl_msg* resp,
                              uint32_t index,
                              int which) {
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts, *entry_list, *entry_nest;
    struct tc_gate gate_params;
    uint32_t interval = GB_SELFTEST_DEFAULT_INTERVAL_NS;
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
    mnl_attr_put_str(nlh, TCA_ACT_KIND, "gate");
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, index);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, GB_SELFTEST_DEFAULT_INTERVAL_NS);

    entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
    entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);

    switch (which) {
        case 0:
            /* Wrong size for INTERVAL */
            {
                size_t before = nlh->nlmsg_len;
                mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
                struct nlattr* attr = (struct nlattr*)((char*)nlh + before);
                attr->nla_len = NLA_HDRLEN + 1;
            }
            break;
        case 1:
            /* Wrong size for IPV */
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            {
                size_t before = nlh->nlmsg_len;
                mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_IPV, 0);
                struct nlattr* attr = (struct nlattr*)((char*)nlh + before);
                attr->nla_len = NLA_HDRLEN + 1;
            }
            break;
        case 2:
            /* Wrong size for MAX_OCTETS */
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            {
                size_t before = nlh->nlmsg_len;
                mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_MAX_OCTETS, 0);
                struct nlattr* attr = (struct nlattr*)((char*)nlh + before);
                attr->nla_len = NLA_HDRLEN + 1;
            }
            break;
        default:
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, interval);
            break;
    }

    mnl_attr_nest_end(nlh, entry_nest);
    mnl_attr_nest_end(nlh, entry_list);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;

    return gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
}

int gb_selftest_invalid_entry_attrs(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    int ret;
    int test_ret = 0;
    bool saw_einval = false;

    ret = gb_selftest_alloc_msgs(&msg, &resp, 1024);
    if (ret < 0) {
        return ret;
    }

    for (int i = 0; i < 3; i++) {
        uint32_t index = base_index + (uint32_t)i;

        ret = send_invalid_entry(sock, msg, resp, index, i);
        if (ret == -EINVAL) {
            saw_einval = true;
        }
        else if (ret != 0) {
            printf("Invalid entry attribute test %d unexpected error %d\n", i, ret);
            test_ret = ret;
            gb_selftest_cleanup_gate(sock, msg, resp, index);
            break;
        }

        gb_selftest_cleanup_gate(sock, msg, resp, index);
    }

    gb_selftest_free_msgs(msg, resp);
    if (test_ret < 0)
        return test_ret;
    return saw_einval ? -EINVAL : 0;
}
