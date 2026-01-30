#include "selftest_tests.h"
#include <errno.h>
#include <string.h>

int gb_selftest_create_missing_parms(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    int ret;

    (void)base_index;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(1, 0));
    if (ret < 0) {
        return ret;
    }

    /* Manually build message without TCA_GATE_PARMS */
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
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    /* SKIP TCA_GATE_PARMS */

    /* Add other attributes to be a "valid" request otherwise */
    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, CLOCK_TAI);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, 0);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, GB_SELFTEST_DEFAULT_INTERVAL_NS);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);

    gb_selftest_free_msgs(msg, resp);
    return ret;
}
