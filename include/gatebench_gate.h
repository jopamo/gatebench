#ifndef GATEBENCH_GATE_H
#define GATEBENCH_GATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_cls.h>
#include <linux/tc_act/tc_gate.h>
#include "gatebench_nl.h"

/* Forward declaration - defined in gatebench.h */
struct gate_shape;

/* Netlink message types for tc actions - check if not already defined */
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
#define TCA_ACT_KIND 1
#define TCA_ACT_OPTIONS 2
#define TCA_ACT_INDEX 3
#endif

#ifndef TCA_OPTIONS
#define TCA_OPTIONS TCA_ACT_OPTIONS
#endif

#ifndef TCA_ACT_TAB
#define TCA_ACT_TAB 1
#endif

/* Use priority slot 1 for action nesting */
#define GATEBENCH_ACT_PRIO 1

/* Gate entry */
struct gate_entry {
    uint32_t index;    /* Entry index (from dump) */
    bool gate_state;   /* Gate state: true=open, false=closed */
    uint32_t interval; /* Interval in nanoseconds */
    int32_t ipv;       /* IP version (-1 for any) */
    int32_t maxoctets; /* Maximum octets (-1 for unlimited) */
};

/* Gate dump structure */
struct gate_dump {
    uint32_t index;
    uint32_t clockid;
    uint64_t base_time;
    uint64_t cycle_time;
    uint64_t cycle_time_ext;
    uint32_t flags;
    int32_t priority;
    struct gate_entry* entries;
    uint32_t num_entries;
    struct tcf_t tm;
    bool has_tm;
    bool has_basic_stats;
    bool has_queue_stats;
    uint64_t bytes;
    uint64_t packets;
    uint32_t drops;
    uint32_t overlimits;
};

/* Calculate message capacity needed for gate action */
size_t gate_msg_capacity(uint32_t entries, uint32_t flags);

/* Build RTM_NEWACTION message for gate */
int build_gate_newaction(struct gb_nl_msg* msg,
                         uint32_t index,
                         const struct gate_shape* shape,
                         const struct gate_entry* entries,
                         uint32_t num_entries,
                         uint16_t nlmsg_flags,
                         uint32_t gate_flags,
                         int32_t priority);

/* Build RTM_DELACTION message */
int build_gate_delaction(struct gb_nl_msg* msg, uint32_t index);

/* Build RTM_GETACTION message */
int build_gate_getaction(struct gb_nl_msg* msg, uint32_t index);
int build_gate_getaction_ex(struct gb_nl_msg* msg, uint32_t index, uint16_t nlmsg_flags);

/* Add a gate entry to message */
int add_gate_entry(struct gb_nl_msg* msg, const struct gate_entry* entry);

/* Free gate dump structure */
void gb_gate_dump_free(struct gate_dump* dump);

/* Parse gate action from netlink message */
int gb_nl_gate_parse(const struct nlmsghdr* nlh, struct gate_dump* dump);

#endif /* GATEBENCH_GATE_H */
