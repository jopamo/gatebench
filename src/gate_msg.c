#include "../include/gatebench.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"

#include <errno.h>
#include <libmnl/libmnl.h>
#include <stdlib.h>
#include <string.h>

static void add_attr_u32(struct nlmsghdr* nlh, uint16_t type, uint32_t value) {
    mnl_attr_put_u32(nlh, type, value);
}

static void add_attr_s32(struct nlmsghdr* nlh, uint16_t type, int32_t value) {
    mnl_attr_put(nlh, type, sizeof(value), &value);
}

static void add_attr_u64(struct nlmsghdr* nlh, uint16_t type, uint64_t value) {
    mnl_attr_put_u64(nlh, type, value);
}

static void add_attr_strz(struct nlmsghdr* nlh, uint16_t type, const char* str) {
    mnl_attr_put_strz(nlh, type, str);
}

static int mnl_attr_cb_copy(const struct nlattr* attr, void* data) {
    const struct nlattr** tb = data;
    unsigned int type = mnl_attr_get_type(attr);

    tb[type] = attr;
    return MNL_CB_OK;
}

/* Approximate message size needed for a gate NEWACTION/REPLACE */
size_t gate_msg_capacity(uint32_t entries, uint32_t flags) {
    size_t cap = 2048;

    (void)flags;

    if (entries > 0) {
        size_t add = (size_t)entries * 96;
        if (add > 1024 * 1024)
            add = 1024 * 1024;
        cap += add;
    }

    if (cap < (size_t)MNL_SOCKET_BUFFER_SIZE)
        cap = (size_t)MNL_SOCKET_BUFFER_SIZE;

    cap = (cap + 4095) & ~4095u;
    return cap;
}

int build_gate_newaction(struct gb_nl_msg* msg,
                         uint32_t index,
                         const struct gate_shape* shape,
                         const struct gate_entry* entries,
                         uint32_t num_entries,
                         uint16_t nlmsg_flags,
                         uint32_t gate_flags,
                         int32_t priority) {
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts;

    if (!msg || !msg->buf || !shape)
        return -EINVAL;

    if (num_entries > 0 && !entries)
        return -EINVAL;

    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | nlmsg_flags;
    nlh->nlmsg_seq = 0;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);

    add_attr_strz(nlh, TCA_ACT_KIND, "gate");
    add_attr_u32(nlh, TCA_ACT_INDEX, index);

    nest_opts = mnl_attr_nest_start(nlh, TCA_ACT_OPTIONS);

    {
        struct tc_gate gate_params;

        memset(&gate_params, 0, sizeof(gate_params));
        gate_params.index = index;
        gate_params.action = TC_ACT_PIPE;

        mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);
    }

    add_attr_u32(nlh, TCA_GATE_CLOCKID, shape->clockid);
    add_attr_u64(nlh, TCA_GATE_BASE_TIME, shape->base_time);
    add_attr_u64(nlh, TCA_GATE_CYCLE_TIME, shape->cycle_time);

    if (shape->cycle_time_ext != 0)
        add_attr_u64(nlh, TCA_GATE_CYCLE_TIME_EXT, shape->cycle_time_ext);

    if (priority != -1)
        add_attr_s32(nlh, TCA_GATE_PRIORITY, priority);

    if (gate_flags != 0)
        add_attr_u32(nlh, TCA_GATE_FLAGS, gate_flags);

    if (num_entries > 0) {
        struct nlattr* entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);

        for (uint32_t i = 0; i < num_entries; i++) {
            struct nlattr* entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);

            if (entries[i].gate_state)
                mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);

            add_attr_u32(nlh, TCA_GATE_ENTRY_INTERVAL, entries[i].interval);
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

int build_gate_delaction(struct gb_nl_msg* msg, uint32_t index) {
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio;

    if (!msg || !msg->buf)
        return -EINVAL;

    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_DELACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = 0;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);

    add_attr_strz(nlh, TCA_ACT_KIND, "gate");
    add_attr_u32(nlh, TCA_ACT_INDEX, index);

    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    return 0;
}

int build_gate_getaction(struct gb_nl_msg* msg, uint32_t index) {
    struct nlmsghdr* nlh;
    struct tcamsg* tca;
    struct nlattr *nest_tab, *nest_prio;

    if (!msg || !msg->buf)
        return -EINVAL;

    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_GETACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = 0;

    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;

    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);

    add_attr_strz(nlh, TCA_ACT_KIND, "gate");
    add_attr_u32(nlh, TCA_ACT_INDEX, index);

    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    msg->len = nlh->nlmsg_len;
    return 0;
}

void gb_gate_dump_free(struct gate_dump* dump) {
    if (!dump)
        return;

    free(dump->entries);
    dump->entries = NULL;
    dump->num_entries = 0;
}

static int parse_gate_entries_cb(const struct nlattr* attr, void* data) {
    struct gate_dump* dump = data;
    struct nlattr* tb[TCA_GATE_ENTRY_MAX + 1] = {NULL};
    struct gate_entry* entry;
    struct gate_entry* new_entries;

    if (mnl_attr_get_type(attr) != TCA_GATE_ONE_ENTRY)
        return MNL_CB_OK;

    if (mnl_attr_parse_nested(attr, mnl_attr_cb_copy, tb) < 0)
        return MNL_CB_ERROR;

    new_entries = realloc(dump->entries, sizeof(*new_entries) * (dump->num_entries + 1));
    if (!new_entries)
        return MNL_CB_ERROR;

    dump->entries = new_entries;
    entry = &dump->entries[dump->num_entries];
    memset(entry, 0, sizeof(*entry));

    entry->gate_state = tb[TCA_GATE_ENTRY_GATE] != NULL;

    if (tb[TCA_GATE_ENTRY_INTERVAL])
        entry->interval = mnl_attr_get_u32(tb[TCA_GATE_ENTRY_INTERVAL]);

    if (tb[TCA_GATE_ENTRY_IPV])
        entry->ipv = (int32_t)mnl_attr_get_u32(tb[TCA_GATE_ENTRY_IPV]);
    else
        entry->ipv = -1;

    if (tb[TCA_GATE_ENTRY_MAX_OCTETS])
        entry->maxoctets = (int32_t)mnl_attr_get_u32(tb[TCA_GATE_ENTRY_MAX_OCTETS]);
    else
        entry->maxoctets = -1;

    dump->num_entries++;
    return MNL_CB_OK;
}

static int parse_gate_options(struct nlattr* attr, struct gate_dump* dump) {
    struct nlattr* tb[TCA_GATE_MAX + 1] = {NULL};

    if (mnl_attr_parse_nested(attr, mnl_attr_cb_copy, tb) < 0)
        return -1;

    if (tb[TCA_GATE_PARMS]) {
        const struct tc_gate* parms = mnl_attr_get_payload(tb[TCA_GATE_PARMS]);
        dump->index = parms->index;
    }

    if (tb[TCA_GATE_CLOCKID])
        dump->clockid = mnl_attr_get_u32(tb[TCA_GATE_CLOCKID]);

    if (tb[TCA_GATE_BASE_TIME])
        dump->base_time = mnl_attr_get_u64(tb[TCA_GATE_BASE_TIME]);

    if (tb[TCA_GATE_CYCLE_TIME])
        dump->cycle_time = mnl_attr_get_u64(tb[TCA_GATE_CYCLE_TIME]);

    if (tb[TCA_GATE_CYCLE_TIME_EXT])
        dump->cycle_time_ext = mnl_attr_get_u64(tb[TCA_GATE_CYCLE_TIME_EXT]);

    if (tb[TCA_GATE_FLAGS])
        dump->flags = mnl_attr_get_u32(tb[TCA_GATE_FLAGS]);

    if (tb[TCA_GATE_PRIORITY])
        dump->priority = (int32_t)mnl_attr_get_u32(tb[TCA_GATE_PRIORITY]);

    if (tb[TCA_GATE_ENTRY_LIST]) {
        if (mnl_attr_parse_nested(tb[TCA_GATE_ENTRY_LIST], parse_gate_entries_cb, dump) < 0)
            return -1;
    }

    return 0;
}

static int parse_action_prio_cb(const struct nlattr* attr, void* data) {
    struct gate_dump* dump = data;
    struct nlattr* tb[TCA_ACT_MAX + 1] = {NULL};

    if (mnl_attr_parse_nested(attr, mnl_attr_cb_copy, tb) < 0)
        return MNL_CB_ERROR;

    if (!tb[TCA_ACT_KIND] || strcmp(mnl_attr_get_str(tb[TCA_ACT_KIND]), "gate") != 0)
        return MNL_CB_OK;

    if (tb[TCA_ACT_INDEX])
        dump->index = mnl_attr_get_u32(tb[TCA_ACT_INDEX]);

    if (tb[TCA_ACT_OPTIONS]) {
        if (parse_gate_options(tb[TCA_ACT_OPTIONS], dump) < 0)
            return MNL_CB_ERROR;
    }

    return MNL_CB_OK;
}

int gb_nl_gate_parse(const struct nlmsghdr* nlh, struct gate_dump* dump) {
    struct tcamsg* tca;
    struct nlattr* tb[TCA_ROOT_MAX + 1] = {NULL};

    if (!nlh || !dump)
        return -EINVAL;

    memset(dump, 0, sizeof(*dump));
    dump->priority = -1;

    tca = mnl_nlmsg_get_payload(nlh);

    if (mnl_attr_parse(nlh, sizeof(*tca), mnl_attr_cb_copy, tb) < 0)
        return -1;

    if (tb[TCA_ACT_TAB]) {
        if (mnl_attr_parse_nested(tb[TCA_ACT_TAB], parse_action_prio_cb, dump) < 0)
            return -1;
    }

    return 0;
}
