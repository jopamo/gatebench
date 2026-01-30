#include "../include/gatebench.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"
#include <libmnl/libmnl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Calculate approximate message size needed */
size_t gate_msg_capacity(uint32_t entries, uint32_t flags) {
    size_t size = 0;
    
    (void)flags; /* Unused for now */
    
    /* Netlink message header */
    size += 16; /* MNL_NLMSG_HDRLEN */
    
    /* tcamsg structure */
    size += sizeof(struct tcamsg);
    
    /* Action header (nested) */
    size += 4;
    size += 4; /* nested */
    
    /* Gate parameters */
    size += 4 + sizeof(struct tc_gate);
    
    /* Gate shape attributes */
    size += 4 + sizeof(uint32_t); /* clockid */
    size += 4 + sizeof(uint64_t); /* base_time */
    size += 4 + sizeof(uint64_t); /* cycle_time */
    
    /* Entry list (nested) */
    size += 4;
    size += 4; /* nested */
    
    /* Each entry: TCA_GATE_ONE_ENTRY nest + gate state flag + 3 attributes */
    size_t entry_size = 4 +  /* TCA_GATE_ONE_ENTRY nest start */
                       4 +  /* TCA_GATE_ENTRY_GATE flag (zero-length attribute) */
                       (4 + sizeof(uint32_t)) * 3; /* interval, ipv, maxoctets */
    size += entries * entry_size;
    
    /* Add some padding for safety */
    size += 256;
    
    return size;
}

static void add_attr_u32(struct nlmsghdr *nlh, uint16_t type, uint32_t value) {
    mnl_attr_put_u32(nlh, type, value);
}

static void add_attr_s32(struct nlmsghdr *nlh, uint16_t type, int32_t value) {
    mnl_attr_put(nlh, type, sizeof(value), &value);
}

static void add_attr_u64(struct nlmsghdr *nlh, uint16_t type, uint64_t value) {
    mnl_attr_put_u64(nlh, type, value);
}

static void add_attr_str(struct nlmsghdr *nlh, uint16_t type, const char *str) {
    mnl_attr_put_str(nlh, type, str);
}

int build_gate_newaction(struct gb_nl_msg *msg,
                         uint32_t index,
                         const struct gate_shape *shape,
                         const struct gate_entry *entries,
                         uint32_t num_entries,
                         uint16_t nlmsg_flags,
                         uint32_t gate_flags) {
    struct nlmsghdr *nlh;
    struct tcamsg *tca;
    struct nlattr *nest_tab, *nest_prio, *nest_opts, *entry_list;
    uint32_t i;
    
    if (!msg || !msg->buf || !shape) {
        return -EINVAL;
    }
    
    if (num_entries > 0 && !entries) {
        return -EINVAL;
    }
    
    /* Start netlink message */
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | nlmsg_flags;
    nlh->nlmsg_seq = 0; /* Will be set by caller */
    
    /* Add tcamsg */
    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;
    
    /* Start nested attribute for actions table */
    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    
    /* Start nested attribute for action at priority 1 */
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    
    /* Add action kind */
    add_attr_str(nlh, TCA_ACT_KIND, "gate");

    /* Add explicit index for act_api visibility */
    add_attr_u32(nlh, TCA_ACT_INDEX, index);
    
    /* Start nested attribute for options */
    nest_opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);
    
    /* Add gate parameters */
    struct tc_gate gate_params;
    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = index;
    gate_params.action = TC_ACT_PIPE;
    
    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);
    
    /* Add gate shape attributes */
    add_attr_u32(nlh, TCA_GATE_CLOCKID, shape->clockid);
    add_attr_u64(nlh, TCA_GATE_BASE_TIME, shape->base_time);
    add_attr_u64(nlh, TCA_GATE_CYCLE_TIME, shape->cycle_time);
    
    /* Add optional attributes with defaults matching iproute2 */
    int32_t gate_prio = -1;      /* Default: wildcard */
    /* Only add priority if not default (following iproute2 pattern) */
    if (gate_prio != -1) {
        add_attr_s32(nlh, TCA_GATE_PRIORITY, gate_prio);
    }
    
    /* Only add flags if non-zero (following iproute2 pattern) */
    if (gate_flags != 0) {
        add_attr_u32(nlh, TCA_GATE_FLAGS, gate_flags);
    }
    
    /* Add entry list if we have entries */
    if (num_entries > 0) {
        entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
        
        for (i = 0; i < num_entries; i++) {
            struct nlattr *entry_nest;
            
            /* Each entry is wrapped in TCA_GATE_ONE_ENTRY nested attribute */
            entry_nest = mnl_attr_nest_start(nlh, TCA_GATE_ONE_ENTRY);
            
            /* Add gate state if open (following iproute2 pattern) */
            if (entries[i].gate_state) {
                mnl_attr_put(nlh, TCA_GATE_ENTRY_GATE, 0, NULL);
            }
            
            add_attr_u32(nlh, TCA_GATE_ENTRY_INTERVAL, entries[i].interval);
            add_attr_s32(nlh, TCA_GATE_ENTRY_IPV, entries[i].ipv);
            add_attr_s32(nlh, TCA_GATE_ENTRY_MAX_OCTETS, entries[i].maxoctets);
            mnl_attr_nest_end(nlh, entry_nest);
        }
        
        mnl_attr_nest_end(nlh, entry_list);
    }
    
    /* End nested attributes */
    mnl_attr_nest_end(nlh, nest_opts); /* TCA_OPTIONS */
    mnl_attr_nest_end(nlh, nest_prio); /* Priority 1 */
    mnl_attr_nest_end(nlh, nest_tab);  /* TCA_ACT_TAB */
    
    /* Update message length */
    msg->len = nlh->nlmsg_len;
    
    return 0;
}

int build_gate_delaction(struct gb_nl_msg *msg, uint32_t index) {
    struct nlmsghdr *nlh;
    struct tcamsg *tca;
    struct nlattr *nest_tab, *nest_prio;
    
    if (!msg || !msg->buf) {
        return -EINVAL;
    }
    
    /* Start netlink message */
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_DELACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = 0; /* Will be set by caller */
    
    /* Add tcamsg */
    tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
    memset(tca, 0, sizeof(*tca));
    tca->tca_family = AF_UNSPEC;
    
    /* Start nested attribute for actions table */
    nest_tab = mnl_attr_nest_start(nlh, TCA_ACT_TAB);
    
    /* Start nested attribute for action at priority 1 */
    nest_prio = mnl_attr_nest_start(nlh, GATEBENCH_ACT_PRIO);
    
    /* Add action kind */
    add_attr_str(nlh, TCA_ACT_KIND, "gate");

    /* Add index attribute */
    add_attr_u32(nlh, TCA_ACT_INDEX, index);
    
    mnl_attr_nest_end(nlh, nest_prio);
    mnl_attr_nest_end(nlh, nest_tab);

    /* Update message length */
    msg->len = nlh->nlmsg_len;
    
    return 0;
}

int add_gate_entry(struct gb_nl_msg *msg,
                   const struct gate_entry *entry) {
    /* This function is meant to be called within the context of
     * building an entry list. It's not standalone.
     */
    (void)msg;
    (void)entry;
    return -ENOSYS; /* Not implemented as standalone */
}
