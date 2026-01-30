#include "../include/gatebench.h"
#include "../include/gatebench_selftest.h"
#include "../include/gatebench_nl.h"
#include "../include/gatebench_gate.h"
#include <libmnl/libmnl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

struct selftest {
    const char* name;
    int (*func)(struct gb_nl_sock* sock, uint32_t base_index);
    int expected_err;
};

static int test_create_missing_parms(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    int ret;

    (void)base_index;

    size_t cap = gate_msg_capacity(1, 0);
    msg = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        ret = -ENOMEM;
        goto out;
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
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, 1000000);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;

    ret = gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return ret;
}

static int test_create_missing_entries(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;
    struct gate_shape shape;
    struct tc_gate gate_params;
    int ret;

    /* Create message without TCA_GATE_ENTRY_LIST */
    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 1000000; /* 1ms */
    shape.interval_ns = 1000000;
    shape.entries = 1;

    size_t cap = gate_msg_capacity(1, 0);
    msg = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        ret = -ENOMEM;
        goto out;
    }

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
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, base_index);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = base_index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, shape.clockid);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, shape.base_time);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, shape.cycle_time);

    /* Deliberately omit TCA_GATE_ENTRY_LIST */
    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;

    ret = gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return ret;
}

static int test_create_empty_entries(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts, *entry_list;
    struct tc_gate gate_params;
    int ret;

    /* Create message with empty entry list */
    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.interval_ns = 1000000;
    shape.entries = 0;

    size_t cap = gate_msg_capacity(1, 0);
    msg = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        ret = -ENOMEM;
        goto out;
    }

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
    mnl_attr_put_u32(nlh, TCA_ACT_INDEX, base_index);
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);

    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = base_index;
    gate_params.action = TC_ACT_PIPE;
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    mnl_attr_put_u32(nlh, TCA_GATE_CLOCKID, shape.clockid);
    mnl_attr_put_u64(nlh, TCA_GATE_BASE_TIME, shape.base_time);
    mnl_attr_put_u64(nlh, TCA_GATE_CYCLE_TIME, shape.cycle_time);

    entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
    mnl_attr_nest_end(nlh, entry_list);

    mnl_attr_nest_end(nlh, nest_opts);
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;

    ret = gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return ret;
}

static int test_create_zero_interval(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    int ret;

    /* Create message with zero interval */
    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.interval_ns = 0; /* Zero interval */
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 0;
    entry.ipv = -1;
    entry.maxoctets = -1;

    size_t cap = gate_msg_capacity(1, 0);
    msg = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        ret = -ENOMEM;
        goto out;
    }

    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return ret;
}

static int test_create_bad_clockid(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    int ret;

    /* Create message with invalid clockid */
    shape.clockid = 999; /* Invalid clock ID */
    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.interval_ns = 1000000;
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 1000000;
    entry.ipv = -1;
    entry.maxoctets = -1;

    size_t cap = gate_msg_capacity(1, 0);
    msg = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        ret = -ENOMEM;
        goto out;
    }

    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return ret;
}

static int test_replace_without_existing(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    size_t cap;
    int ret;

    /* First delete to ensure it doesn't exist */
    msg = gb_nl_msg_alloc(1024);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        ret = -ENOMEM;
        goto out;
    }

    ret = build_gate_delaction(msg, base_index);
    if (ret < 0) {
        goto out;
    }

    /* Try to delete non-existing - ignore error */
    gb_nl_send_recv(sock, msg, resp, 1000);

    /* Now try to replace non-existing gate */
    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.interval_ns = 1000000;
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 1000000;
    entry.ipv = -1;
    entry.maxoctets = -1;

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

    /* Send with REPLACE flag */
    ret = gb_nl_send_recv(sock, msg, resp, 1000);
    if (ret == 0) {
        /* Created successfully, now delete it */
        gb_nl_msg_reset(msg);
        ret = build_gate_delaction(msg, base_index);
        if (ret >= 0) {
            gb_nl_send_recv(sock, msg, resp, 1000);
        }
        ret = 0;
    }

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return ret;
}

static int test_duplicate_create(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    size_t cap;
    int ret;
    int test_ret = 0;

    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.interval_ns = 1000000;
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 1000000;
    entry.ipv = -1;
    entry.maxoctets = -1;

    cap = gate_msg_capacity(1, 0);
    msg = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        test_ret = -ENOMEM;
        goto out;
    }

    /* First create */
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);
    if (ret < 0 && ret != -EEXIST) {
        test_ret = ret;
        goto cleanup;
    }

    /* Try to create again - should fail with -EEXIST */
    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    test_ret = gb_nl_send_recv(sock, msg, resp, 1000);

cleanup:
    /* Clean up */
    gb_nl_msg_reset(msg);
    ret = build_gate_delaction(msg, base_index);
    if (ret >= 0) {
        gb_nl_send_recv(sock, msg, resp, 1000);
    }

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return test_ret;
}

static int test_dump_correctness(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    shape.clockid = CLOCK_TAI;
    shape.base_time = 12345678;
    shape.cycle_time = 1000000;
    shape.interval_ns = 1000000;
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 1000000;
    entry.ipv = 4;
    entry.maxoctets = 1024;

    msg = gb_nl_msg_alloc(gate_msg_capacity(1, 0));
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        test_ret = -ENOMEM;
        goto out;
    }

    /* Create gate */
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    /* Dump and verify */
    ret = gb_nl_get_action(sock, base_index, &dump, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.index != base_index || dump.clockid != shape.clockid || dump.base_time != shape.base_time ||
        dump.cycle_time != shape.cycle_time || dump.num_entries != 1 ||
        dump.entries[0].gate_state != entry.gate_state || dump.entries[0].interval != entry.interval ||
        dump.entries[0].ipv != entry.ipv || dump.entries[0].maxoctets != entry.maxoctets) {
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_nl_msg_reset(msg);
    build_gate_delaction(msg, base_index);
    gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return test_ret;
}

static int test_replace_persistence(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.interval_ns = 1000000;
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 1000000;
    entry.ipv = -1;
    entry.maxoctets = -1;

    msg = gb_nl_msg_alloc(gate_msg_capacity(1, 0));
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        test_ret = -ENOMEM;
        goto out;
    }

    /* 1. Create gate with flags=1, priority=10 */
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 1, 10);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    /* Verify initial values */
    ret = gb_nl_get_action(sock, base_index, &dump, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }
    if (dump.flags != 1 || dump.priority != 10) {
        test_ret = -EINVAL;
        gb_gate_dump_free(&dump);
        goto cleanup;
    }
    gb_gate_dump_free(&dump);

    /* 2. Replace gate with flags=2, priority=20 */
    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_REPLACE, 2, 20);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    /* Verify updated values */
    ret = gb_nl_get_action(sock, base_index, &dump, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }
    if (dump.flags != 2 || dump.priority != 20) {
        test_ret = -EINVAL;
        gb_gate_dump_free(&dump);
        goto cleanup;
    }
    gb_gate_dump_free(&dump);

cleanup:
    gb_nl_msg_reset(msg);
    build_gate_delaction(msg, base_index);
    gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return test_ret;
}

static int test_clockid_variants(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    /* Valid clock IDs to test */
    uint32_t valid_clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_TAI};
    size_t i;

    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.interval_ns = 1000000;
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 1000000;
    entry.ipv = -1;
    entry.maxoctets = -1;

    msg = gb_nl_msg_alloc(gate_msg_capacity(1, 0));
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        test_ret = -ENOMEM;
        goto out;
    }

    for (i = 0; i < sizeof(valid_clocks) / sizeof(valid_clocks[0]); i++) {
        uint32_t clockid = valid_clocks[i];

        /* Cleanup previous if any */
        gb_nl_msg_reset(msg);
        build_gate_delaction(msg, base_index);
        gb_nl_send_recv(sock, msg, resp, 1000);

        /* Create with specific clockid */
        shape.clockid = clockid;
        gb_nl_msg_reset(msg);
        ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
        if (ret < 0) {
            test_ret = ret;
            goto out;
        }

        ret = gb_nl_send_recv(sock, msg, resp, 1000);
        if (ret < 0) {
            printf("Failed to create with clockid %u: %d\n", clockid, ret);
            test_ret = ret;
            goto cleanup;
        }

        /* Verify dump */
        ret = gb_nl_get_action(sock, base_index, &dump, 1000);
        if (ret < 0) {
            test_ret = ret;
            goto cleanup;
        }

        if (dump.clockid != clockid) {
            printf("Clock ID mismatch: expected %u, got %u\n", clockid, dump.clockid);
            test_ret = -EINVAL;
            gb_gate_dump_free(&dump);
            goto cleanup;
        }
        gb_gate_dump_free(&dump);
    }

cleanup:
    gb_nl_msg_reset(msg);
    build_gate_delaction(msg, base_index);
    gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return test_ret;
}

static int test_cycle_time_derivation(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entries[2];
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 0; /* Request derivation */
    shape.entries = 2;

    entries[0].gate_state = true;
    entries[0].interval = 500000; /* 0.5ms */
    entries[0].ipv = -1;
    entries[0].maxoctets = -1;

    entries[1].gate_state = false;
    entries[1].interval = 1500000; /* 1.5ms */
    entries[1].ipv = -1;
    entries[1].maxoctets = -1;

    msg = gb_nl_msg_alloc(gate_msg_capacity(2, 0));
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        test_ret = -ENOMEM;
        goto out;
    }

    /* Create gate with cycle_time=0 */
    ret = build_gate_newaction(msg, base_index, &shape, entries, 2, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    /* Verify derived cycle_time (should be 500000 + 1500000 = 2000000) */
    ret = gb_nl_get_action(sock, base_index, &dump, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.cycle_time != 2000000) {
        printf("Cycle time derivation failed: expected 2000000, got %lu\n", dump.cycle_time);
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_nl_msg_reset(msg);
    build_gate_delaction(msg, base_index);
    gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return test_ret;
}

static int test_cycle_time_ext_parsing(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry entry;
    struct gate_dump dump;
    int ret;
    int test_ret = 0;

    shape.clockid = CLOCK_TAI;
    shape.base_time = 0;
    shape.cycle_time = 1000000;
    shape.cycle_time_ext = 500000; /* 0.5ms extension */
    shape.entries = 1;

    entry.gate_state = true;
    entry.interval = 1000000;
    entry.ipv = -1;
    entry.maxoctets = -1;

    msg = gb_nl_msg_alloc(gate_msg_capacity(1, 0));
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!msg || !resp) {
        test_ret = -ENOMEM;
        goto out;
    }

    /* Create gate with cycle_time_ext */
    ret = build_gate_newaction(msg, base_index, &shape, &entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    ret = gb_nl_send_recv(sock, msg, resp, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    /* Verify dump */
    ret = gb_nl_get_action(sock, base_index, &dump, 1000);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    if (dump.cycle_time_ext != shape.cycle_time_ext) {
        printf("Cycle time extension parsing failed: expected %lu, got %lu\n", shape.cycle_time_ext,
               dump.cycle_time_ext);
        test_ret = -EINVAL;
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_nl_msg_reset(msg);
    build_gate_delaction(msg, base_index);
    gb_nl_send_recv(sock, msg, resp, 1000);

out:
    if (msg)
        gb_nl_msg_free(msg);
    if (resp)
        gb_nl_msg_free(resp);
    return test_ret;
}

static const struct selftest tests[] = {
    {"create missing parms", test_create_missing_parms, -EINVAL},
    {"create missing entry list", test_create_missing_entries, -EINVAL},
    {"create empty entry list", test_create_empty_entries, -EINVAL},
    {"create zero interval", test_create_zero_interval, -EINVAL},
    {"create bad clockid", test_create_bad_clockid, -EINVAL},
    {"replace without existing", test_replace_without_existing, 0},
    {"duplicate create", test_duplicate_create, -EEXIST},
    {"dump correctness", test_dump_correctness, 0},
    {"replace persistence", test_replace_persistence, 0},
    {"clockid variants", test_clockid_variants, 0},
    {"cycle time derivation", test_cycle_time_derivation, 0},
    {"cycle time extension parsing", test_cycle_time_ext_parsing, 0},
};
#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

int gb_selftest_run(const struct gb_config* cfg) {
    struct gb_nl_sock* sock = NULL;
    uint32_t base_index;
    int ret;
    size_t i;
    int passed = 0;

    /* Open netlink socket */
    ret = gb_nl_open(&sock);
    if (ret < 0) {
        fprintf(stderr, "Failed to open netlink socket: %s\n", strerror(-ret));
        return ret;
    }

    /* Use configured index as base */
    base_index = cfg->index;

    printf("Running %zu selftests...\n", NUM_TESTS);

    for (i = 0; i < NUM_TESTS; i++) {
        uint32_t test_index = base_index + (uint32_t)(i * 1024); /* Space tests apart */

        printf("  %-30s ", tests[i].name);
        fflush(stdout);

        ret = tests[i].func(sock, test_index);

        if (gb_nl_error_expected(ret, tests[i].expected_err)) {
            printf("PASS (got %d)\n", ret);
            passed++;
        }
        else {
            printf("FAIL (got %d, expected %d)\n", ret, tests[i].expected_err);
        }
    }

    printf("\nSelftests: %d/%zu passed\n", passed, NUM_TESTS);

    gb_nl_close(sock);

    if (passed == NUM_TESTS) {
        return 0;
    }
    else {
        return -EINVAL;
    }
}
