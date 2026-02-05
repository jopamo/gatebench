#include "selftest_tests.h"
#include <errno.h>
#include <string.h>

enum attr_bits {
    ATTR_CLOCKID = 1u << 0,
    ATTR_BASE_TIME = 1u << 1,
    ATTR_CYCLE_TIME = 1u << 2,
    ATTR_CYCLE_TIME_EXT = 1u << 3,
    ATTR_FLAGS = 1u << 4,
    ATTR_PRIORITY = 1u << 5,
    ATTR_ENTRIES = 1u << 6,
};

static uint64_t sum_intervals(const struct gate_entry* entries, uint32_t num_entries) {
    uint64_t total = 0;

    for (uint32_t i = 0; i < num_entries; i++) {
        total += entries[i].interval;
    }

    return total;
}

static void add_attr_s32(struct nlmsghdr* nlh, uint16_t type, int32_t value) {
    mnl_attr_put(nlh, type, sizeof(value), &value);
}

static int build_gate_replace_mask(struct gb_nl_msg* msg,
                                   uint32_t index,
                                   const struct gate_shape* shape,
                                   const struct gate_entry* entries,
                                   uint32_t num_entries,
                                   uint32_t gate_flags,
                                   int32_t priority,
                                   uint32_t attr_mask,
                                   bool add_unknown,
                                   bool add_unknown_entry) {
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct tc_gate gate_params;

    if (!msg || !msg->buf || !shape)
        return -EINVAL;

    if ((attr_mask & ATTR_ENTRIES) && (!entries || num_entries == 0))
        return -EINVAL;

    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_REPLACE;
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

    if (attr_mask & ATTR_CLOCKID)
        mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, shape->clockid);
    if (attr_mask & ATTR_BASE_TIME)
        mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, shape->base_time);
    if (attr_mask & ATTR_CYCLE_TIME)
        mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, shape->cycle_time);
    if (attr_mask & ATTR_CYCLE_TIME_EXT)
        mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME_EXT, shape->cycle_time_ext);
    if (attr_mask & ATTR_PRIORITY)
        add_attr_s32(nlh, TCA_GATE_PRIORITY, priority);
    if (attr_mask & ATTR_FLAGS)
        mnl_attr_put_u32(nlh, TCA_GATE_FLAGS, gate_flags);

    if (attr_mask & ATTR_ENTRIES) {
        struct nlattr* entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
        for (uint32_t i = 0; i < num_entries; i++) {
            struct nlattr* entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);
            if (entries[i].gate_state)
                mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);
            mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_INTERVAL, entries[i].interval);
            add_attr_s32(nlh, TCA_GATE_ENTRY_IPV, entries[i].ipv);
            add_attr_s32(nlh, TCA_GATE_ENTRY_MAX_OCTETS, entries[i].maxoctets);
            if (add_unknown_entry)
                mnl_attr_put_u32(nlh, TCA_GATE_ENTRY_MAX + 1, 0xdeadbeefU);
            mnl_attr_nest_end(nlh, entry_nest);
        }
        mnl_attr_nest_end(nlh, entry_list);
    }

    if (add_unknown)
        mnl_attr_put_u32(nlh, TCA_GATE_MAX + 1, 0xcafebabeU);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    return 0;
}

static int verify_dump(uint32_t mask,
                       const struct gate_dump* dump,
                       uint32_t exp_clockid,
                       uint64_t exp_base_time,
                       uint64_t exp_cycle_time,
                       uint64_t exp_cycle_time_ext,
                       uint32_t exp_flags,
                       int32_t exp_priority,
                       const struct gate_entry* exp_entries,
                       uint32_t exp_num_entries) {
    if (dump->clockid != exp_clockid || dump->base_time != exp_base_time || dump->cycle_time != exp_cycle_time ||
        dump->cycle_time_ext != exp_cycle_time_ext || dump->flags != exp_flags || dump->priority != exp_priority ||
        dump->num_entries != exp_num_entries) {
        gb_selftest_log(
            "mask 0x%02x mismatch: clock=%u/%u base=%lu/%lu cycle=%lu/%lu ext=%lu/%lu flags=%u/%u prio=%d/%d "
            "entries=%u/%u\n",
            mask, dump->clockid, exp_clockid, dump->base_time, exp_base_time, dump->cycle_time, exp_cycle_time,
            dump->cycle_time_ext, exp_cycle_time_ext, dump->flags, exp_flags, dump->priority, exp_priority,
            dump->num_entries, exp_num_entries);
        return -EINVAL;
    }

    for (uint32_t i = 0; i < exp_num_entries; i++) {
        if (dump->entries[i].gate_state != exp_entries[i].gate_state ||
            dump->entries[i].interval != exp_entries[i].interval || dump->entries[i].ipv != exp_entries[i].ipv ||
            dump->entries[i].maxoctets != exp_entries[i].maxoctets) {
            gb_selftest_log("mask 0x%02x entry %u mismatch\n", mask, i);
            return -EINVAL;
        }
    }

    return 0;
}

int gb_selftest_attr_matrix(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape old_shape;
    struct gate_shape new_shape;
    struct gate_entry old_entry;
    struct gate_entry new_entries[2];
    struct gate_dump dump;
    int ret;
    int test_ret = 0;
    const uint32_t old_flags = 0x11U;
    const uint32_t new_flags = 0x22U;
    const int32_t old_priority = 5;
    const int32_t new_priority = 9;
    const uint32_t mask_max = (1u << 7);

    gb_selftest_shape_default(&old_shape, 1);
    old_shape.clockid = CLOCK_MONOTONIC;
    old_shape.base_time = 1111;
    old_shape.cycle_time = 1000000;
    old_shape.cycle_time_ext = 2222;
    gb_selftest_entry_default(&old_entry);
    old_entry.interval = 1000000;
    old_entry.gate_state = true;

    gb_selftest_shape_default(&new_shape, 2);
    new_shape.clockid = CLOCK_BOOTTIME;
    new_shape.base_time = 3333;
    new_shape.cycle_time = 9000000;
    new_shape.cycle_time_ext = 4444;
    gb_selftest_entry_default(&new_entries[0]);
    new_entries[0].gate_state = false;
    new_entries[0].interval = 2000000;
    new_entries[0].ipv = -1;
    new_entries[0].maxoctets = -1;
    gb_selftest_entry_default(&new_entries[1]);
    new_entries[1].gate_state = true;
    new_entries[1].interval = 3000000;
    new_entries[1].ipv = -1;
    new_entries[1].maxoctets = -1;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(2, new_flags));
    if (ret < 0)
        return ret;

    for (uint32_t mask = 0; mask < mask_max; mask++) {
        uint32_t exp_clockid;
        uint64_t exp_base_time;
        uint64_t exp_cycle_time;
        uint64_t exp_cycle_time_ext;
        uint32_t exp_flags;
        int32_t exp_priority;
        const struct gate_entry* exp_entries = &old_entry;
        uint32_t exp_num_entries = 1;

        gb_nl_msg_reset(msg);
        ret = build_gate_newaction(msg, base_index, &old_shape, &old_entry, 1, NLM_F_CREATE | NLM_F_EXCL, old_flags,
                                   old_priority);
        if (ret < 0) {
            test_ret = ret;
            break;
        }
        ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            test_ret = ret;
            gb_selftest_cleanup_gate(sock, msg, resp, base_index);
            break;
        }

        gb_nl_msg_reset(msg);
        ret = build_gate_replace_mask(msg, base_index, &new_shape, new_entries, 2, new_flags, new_priority, mask, false,
                                      false);
        if (ret < 0) {
            test_ret = ret;
            gb_selftest_cleanup_gate(sock, msg, resp, base_index);
            break;
        }
        ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            test_ret = ret;
            gb_selftest_cleanup_gate(sock, msg, resp, base_index);
            break;
        }

        ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            test_ret = ret;
            gb_selftest_cleanup_gate(sock, msg, resp, base_index);
            break;
        }

        exp_clockid = (mask & ATTR_CLOCKID) ? new_shape.clockid : old_shape.clockid;
        exp_base_time = (mask & ATTR_BASE_TIME) ? new_shape.base_time : old_shape.base_time;
        exp_cycle_time_ext = (mask & ATTR_CYCLE_TIME_EXT) ? new_shape.cycle_time_ext : old_shape.cycle_time_ext;
        exp_flags = (mask & ATTR_FLAGS) ? new_flags : old_flags;
        exp_priority = (mask & ATTR_PRIORITY) ? new_priority : old_priority;

        if (mask & ATTR_ENTRIES) {
            exp_entries = new_entries;
            exp_num_entries = 2;
        }

        if (mask & ATTR_CYCLE_TIME)
            exp_cycle_time = new_shape.cycle_time;
        else if (mask & ATTR_ENTRIES)
            exp_cycle_time = sum_intervals(new_entries, 2);
        else
            exp_cycle_time = old_shape.cycle_time;

        ret = verify_dump(mask, &dump, exp_clockid, exp_base_time, exp_cycle_time, exp_cycle_time_ext, exp_flags,
                          exp_priority, exp_entries, exp_num_entries);
        gb_gate_dump_free(&dump);
        gb_selftest_cleanup_gate(sock, msg, resp, base_index);
        if (ret < 0) {
            test_ret = ret;
            break;
        }
    }

    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}

int gb_selftest_unknown_attrs(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape old_shape;
    struct gate_shape new_shape;
    struct gate_entry old_entry;
    struct gate_entry new_entries[2];
    struct gate_dump dump;
    int ret;
    int test_ret = 0;
    const uint32_t old_flags = 0x10U;
    const uint32_t new_flags = 0x20U;
    const int32_t old_priority = 3;
    const int32_t new_priority = 7;
    const uint32_t full_mask = ATTR_CLOCKID | ATTR_BASE_TIME | ATTR_CYCLE_TIME | ATTR_CYCLE_TIME_EXT | ATTR_FLAGS |
                               ATTR_PRIORITY | ATTR_ENTRIES;

    gb_selftest_shape_default(&old_shape, 1);
    old_shape.clockid = CLOCK_TAI;
    old_shape.base_time = 5555;
    old_shape.cycle_time = 7000000;
    old_shape.cycle_time_ext = 0;
    gb_selftest_entry_default(&old_entry);
    old_entry.interval = 7000000;

    gb_selftest_shape_default(&new_shape, 2);
    new_shape.clockid = CLOCK_REALTIME;
    new_shape.base_time = 6666;
    new_shape.cycle_time = 8000000;
    new_shape.cycle_time_ext = 9999;
    gb_selftest_entry_default(&new_entries[0]);
    new_entries[0].interval = 4000000;
    new_entries[0].gate_state = true;
    gb_selftest_entry_default(&new_entries[1]);
    new_entries[1].interval = 4000000;
    new_entries[1].gate_state = false;

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(2, new_flags));
    if (ret < 0)
        return ret;

    ret = build_gate_newaction(msg, base_index, &old_shape, &old_entry, 1, NLM_F_CREATE | NLM_F_EXCL, old_flags,
                               old_priority);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    gb_nl_msg_reset(msg);
    ret = build_gate_replace_mask(msg, base_index, &new_shape, new_entries, 2, new_flags, new_priority, full_mask, true,
                                  true);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
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

    ret = verify_dump(full_mask, &dump, new_shape.clockid, new_shape.base_time, new_shape.cycle_time,
                      new_shape.cycle_time_ext, new_flags, new_priority, new_entries, 2);
    gb_gate_dump_free(&dump);
    if (ret < 0)
        test_ret = ret;

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);
out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
