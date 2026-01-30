#include "../include/gatebench.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"
#include <libmnl/libmnl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Default netlink attributes */
#define TCA_ACT_MAX_PRIO 1

/* RTM types for tc actions - check if not already defined */
#ifndef RTM_NEWACTION
#define RTM_NEWACTION 48
#define RTM_DELACTION 49
#define RTM_GETACTION 50
#endif

    /* tc action constants */
#ifndef TC_ACT_PIPE
#define TC_ACT_PIPE 3
#endif

/* Netlink attribute types - check if not already defined */
#ifndef TCA_ACT_KIND
#define TCA_ACT_KIND     1
#define TCA_ACT_OPTIONS  2
#define TCA_ACT_INDEX    3
#endif

#ifndef TCA_OPTIONS
#define TCA_OPTIONS TCA_ACT_OPTIONS
#endif

/* Gate-specific attribute types - from linux/tc_act/tc_gate.h */
#ifndef TCA_GATE_PARMS
#define TCA_GATE_PARMS          2
#define TCA_GATE_PAD            3
#define TCA_GATE_PRIORITY       4
#define TCA_GATE_ENTRY_LIST     6
#define TCA_GATE_BASE_TIME      7
#define TCA_GATE_CYCLE_TIME     8
#define TCA_GATE_CYCLE_TIME_EXT 9
#define TCA_GATE_FLAGS          10
#define TCA_GATE_CLOCKID        11
#endif

/* Gate entry attribute types - from linux/tc_act/tc_gate.h */
#ifndef TCA_GATE_ENTRY_UNSPEC
#define TCA_GATE_ENTRY_UNSPEC     0
#define TCA_GATE_ENTRY_INDEX      1
#define TCA_GATE_ENTRY_GATE       2
#define TCA_GATE_ENTRY_INTERVAL   3
#define TCA_GATE_ENTRY_IPV        4
#define TCA_GATE_ENTRY_MAX_OCTETS 5
#endif

/* TCA_GATE_ONE_ENTRY for nested entry attributes */
#ifndef TCA_GATE_ONE_ENTRY
#define TCA_GATE_ONE_ENTRY       1
#endif

/* tc_gate structure (simplified) */
struct tc_gate {
    uint32_t index;
    uint32_t capab;
    int      action;
    int      refcnt;
    int      bindcnt;
};

/* tcmsg structure - in case it's not defined */
struct tcmsg {
    unsigned char tcm_family;
    unsigned char tcm_pad1;
    unsigned char tcm_pad2;
    unsigned char tcm_pad3;
    uint32_t tcm_ifindex;
    uint32_t tcm_handle;
    uint32_t tcm_parent;
    uint32_t tcm_info;
};

/* tc constants if not defined */
#ifndef TC_H_ROOT
#define TC_H_ROOT       0xFFFFFFFFU
#endif

#ifndef TC_H_MAKE
#define TC_H_MAKE(maj, min) (((maj) << 16) | (min))
#endif

/* Calculate approximate message size needed */
size_t gate_msg_capacity(uint32_t entries, uint32_t flags) {
    size_t size = 0;
    
    (void)flags; /* Unused for now */
    
    /* Netlink message header */
    size += 16; /* MNL_NLMSG_HDRLEN */
    
    /* tcmsg structure */
    size += sizeof(struct tcmsg);
    
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
                          uint32_t flags) {
    /* flags parameter is used for TCA_GATE_FLAGS */
    struct nlmsghdr *nlh;
    struct tcmsg *tcm;
    struct nlattr *nest, *entry_list;
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
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE;
    nlh->nlmsg_seq = 0; /* Will be set by caller */
    
    /* Add tcmsg */
    tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
    memset(tcm, 0, sizeof(*tcm));
    tcm->tcm_family = AF_UNSPEC;
    tcm->tcm_ifindex = 0; /* No interface */
    tcm->tcm_parent = TC_H_ROOT;
    tcm->tcm_info = TC_H_MAKE(0, 0);
    
    /* Start nested attribute for action */
    nest = mnl_attr_nest_start(nlh, TCA_ACT_MAX_PRIO);
    
    /* Add action kind */
    add_attr_str(nlh, TCA_ACT_KIND, "gate");
    
    /* Start nested attribute for options */
    nest = mnl_attr_nest_start(nlh, TCA_OPTIONS);
    
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
    uint32_t gate_flags = flags; /* Use passed flags parameter */
    
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
    mnl_attr_nest_end(nlh, nest); /* TCA_OPTIONS */
    mnl_attr_nest_end(nlh, nest); /* TCA_ACT_MAX_PRIO */
    
    /* Update message length */
    msg->len = nlh->nlmsg_len;
    
    return 0;
}

int build_gate_delaction(struct gb_nl_msg *msg, uint32_t index) {
    struct nlmsghdr *nlh;
    struct tcmsg *tcm;
    
    if (!msg || !msg->buf) {
        return -EINVAL;
    }
    
    /* Start netlink message */
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_DELACTION;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = 0; /* Will be set by caller */
    
    /* Add tcmsg */
    tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
    memset(tcm, 0, sizeof(*tcm));
    tcm->tcm_family = AF_UNSPEC;
    tcm->tcm_ifindex = 0; /* No interface */
    tcm->tcm_parent = TC_H_ROOT;
    tcm->tcm_info = TC_H_MAKE(0, 0);
    
    /* Add index attribute */
    add_attr_u32(nlh, TCA_ACT_INDEX, index);
    
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