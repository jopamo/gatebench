#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int gb_selftest_replace_preserve_attrs(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_entry entries[2];
    struct gate_dump dump;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;
    int ret;
    int test_ret = 0;
    const uint32_t gate_flags = 0x5a5aU;
    const int32_t priority = 7;
    const uint64_t base_time = 1234567;
    const uint32_t clockid = CLOCK_MONOTONIC;

    gb_selftest_shape_default(&shape, 1);
    shape.clockid = clockid;
    shape.base_time = base_time;
    shape.cycle_time = 1000000;
    gb_selftest_entry_default(&entry);

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(2, gate_flags));
    if (ret < 0) {
        return ret;
    }

    /* 1. Create gate with non-default attributes. */
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, gate_flags, priority);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    /* 2. Replace with new entries but omit base_time/clockid/flags/priority. */
    gb_selftest_entry_default(&entries[0]);
    entries[0].gate_state = false;
    entries[0].interval = 500000;
    entries[0].ipv = -1;
    entries[0].maxoctets = -1;
    gb_selftest_entry_default(&entries[1]);
    entries[1].gate_state = true;
    entries[1].interval = 750000;
    entries[1].ipv = -1;
    entries[1].maxoctets = -1;

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

    {
        struct nlattr* entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
        for (size_t i = 0; i < 2; i++) {
            struct nlattr* entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);
            if (entries[i].gate_state)
                mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, entries[i].interval);
            mnl_attr_put(nlh, TCA_GATE_ENTRY_IPV, sizeof(entries[i].ipv), &entries[i].ipv);
            mnl_attr_put(nlh, TCA_GATE_ENTRY_MAX_OCTETS, sizeof(entries[i].maxoctets), &entries[i].maxoctets);
            mnl_attr_nest_end(nlh, entry_nest);
        }
        mnl_attr_nest_end(nlh, entry_list);
    }

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    /* 3. Verify preserved attributes and new entry list. */
    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.base_time != base_time || dump.clockid != clockid || dump.flags != gate_flags ||
        dump.priority != priority || dump.num_entries != 2) {
        gb_selftest_log("Replace preserved attrs failed: base=%lu clock=%u flags=%u prio=%d entries=%u\n",
                        dump.base_time, dump.clockid, dump.flags, dump.priority, dump.num_entries);
        test_ret = -EINVAL;
        gb_gate_dump_free(&dump);
        goto cleanup;
    }

    if (dump.entries[0].gate_state != entries[0].gate_state || dump.entries[0].interval != entries[0].interval ||
        dump.entries[1].gate_state != entries[1].gate_state || dump.entries[1].interval != entries[1].interval) {
        gb_selftest_log("Replace entry list mismatch: entry0=%u/%u entry1=%u/%u\n", dump.entries[0].gate_state,
                        dump.entries[0].interval, dump.entries[1].gate_state, dump.entries[1].interval);
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
