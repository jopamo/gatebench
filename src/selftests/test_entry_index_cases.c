#include "selftest_tests.h"
#include <errno.h>
#include <string.h>

static void add_attr_s32(struct nlmsghdr* nlh, uint16_t type, int32_t value) {
    mnl_attr_put(nlh, type, sizeof(value), &value);
}

static int build_gate_create_custom(struct gb_nl_msg* msg,
                                    uint32_t index,
                                    const struct gate_shape* shape,
                                    const struct gate_entry* entries,
                                    const uint32_t* entry_indices,
                                    uint32_t num_entries,
                                    bool include_index_attr,
                                    int invalid_interval_index) {
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;

    if (!msg || !msg->buf || !shape || !entries)
        return -EINVAL;

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
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, index);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, shape->clockid);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, shape->base_time);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, shape->cycle_time);
    if (shape->cycle_time_ext != 0)
        mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME_EXT, shape->cycle_time_ext);

    {
        struct nlattr* entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
        for (uint32_t i = 0; i < num_entries; i++) {
            struct nlattr* entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);
            if (include_index_attr && entry_indices)
                mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INDEX, entry_indices[i]);
            if (entries[i].gate_state)
                mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);
            if ((int)i != invalid_interval_index)
                mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, entries[i].interval);
            add_attr_s32(nlh, TCA_GATE_ENTRY_IPV, entries[i].ipv);
            add_attr_s32(nlh, TCA_GATE_ENTRY_MAX_OCTETS, entries[i].maxoctets);
            mnl_attr_nest_end(nlh, entry_nest);
        }
        mnl_attr_nest_end(nlh, entry_list);
    }

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    return 0;
}

int gb_selftest_entry_index_attrs(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entries[3];
    struct gate_dump dump;
    uint32_t entry_indices[3] = {5, 5, 2};
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 3);
    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 6000000;
    shape.cycle_time_ext = 0;

    gb_selftest_entry_default(&entries[0]);
    entries[0].gate_state = true;
    entries[0].interval = 1000000;
    gb_selftest_entry_default(&entries[1]);
    entries[1].gate_state = false;
    entries[1].interval = 2000000;
    gb_selftest_entry_default(&entries[2]);
    entries[2].gate_state = true;
    entries[2].interval = 3000000;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(3, 0));
    if (ret < 0)
        return ret;

    gb_nl_msg_reset(msg);
    ret = build_gate_create_custom(msg, base_index, &shape, entries, entry_indices, 3, true, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.num_entries != 3) {
        gb_selftest_log("Entry index attrs failed: expected 3 entries, got %u\n", dump.num_entries);
        test_ret = -EINVAL;
        gb_gate_dump_free(&dump);
        goto cleanup;
    }

    for (uint32_t i = 0; i < 3; i++) {
        if (dump.entries[i].interval != entries[i].interval || dump.entries[i].gate_state != entries[i].gate_state) {
            gb_selftest_log("Entry index attrs order mismatch at %u\n", i);
            test_ret = -EINVAL;
            break;
        }
        if (dump.entries[i].index != i) {
            gb_selftest_log("Entry index attrs not normalized: %u -> %u\n", i, dump.entries[i].index);
            test_ret = -EINVAL;
            break;
        }
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);
out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}

int gb_selftest_mixed_invalid_entries(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entries[3];
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    gb_selftest_shape_default(&shape, 3);
    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 6000000;

    gb_selftest_entry_default(&entries[0]);
    entries[0].gate_state = true;
    entries[0].interval = 1000000;
    gb_selftest_entry_default(&entries[1]);
    entries[1].gate_state = false;
    entries[1].interval = 0;
    gb_selftest_entry_default(&entries[2]);
    entries[2].gate_state = true;
    entries[2].interval = 3000000;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(3, 0));
    if (ret < 0)
        return ret;

    gb_nl_msg_reset(msg);
    ret = build_gate_create_custom(msg, base_index, &shape, entries, NULL, 3, false, 1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret == 0) {
        gb_selftest_log("Mixed invalid/valid entries should fail\n");
        test_ret = -EINVAL;
        goto cleanup;
    }

    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret == 0) {
        gb_selftest_log("Mixed invalid/valid entries unexpectedly created action\n");
        gb_gate_dump_free(&dump);
        test_ret = -EINVAL;
    }

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);
out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
