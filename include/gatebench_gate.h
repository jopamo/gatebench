#ifndef GATEBENCH_GATE_H
#define GATEBENCH_GATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_cls.h>
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
#define TCA_ACT_KIND     1
#define TCA_ACT_OPTIONS  2
#define TCA_ACT_INDEX    3
#endif

#ifndef TCA_OPTIONS
#define TCA_OPTIONS TCA_ACT_OPTIONS
#endif

#ifndef TCA_ACT_TAB
#define TCA_ACT_TAB 1
#endif

/* Use priority slot 1 for action nesting */
#define GATEBENCH_ACT_PRIO 1

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

/* Gate entry */
struct gate_entry {
    bool gate_state;    /* Gate state: true=open, false=closed */
    uint32_t interval;  /* Interval in nanoseconds */
    int32_t ipv;        /* IP version (-1 for any) */
    int32_t maxoctets;  /* Maximum octets (-1 for unlimited) */
};

/* Calculate message capacity needed for gate action */
size_t gate_msg_capacity(uint32_t entries, uint32_t flags);

/* Build RTM_NEWACTION message for gate */
int build_gate_newaction(struct gb_nl_msg *msg,
                         uint32_t index,
                         const struct gate_shape *shape,
                         const struct gate_entry *entries,
                         uint32_t num_entries,
                         uint16_t nlmsg_flags,
                         uint32_t gate_flags);

/* Build RTM_DELACTION message */
int build_gate_delaction(struct gb_nl_msg *msg, uint32_t index);

/* Add a gate entry to message */
int add_gate_entry(struct gb_nl_msg *msg,
                   const struct gate_entry *entry);

#endif /* GATEBENCH_GATE_H */
