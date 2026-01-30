#ifndef GATEBENCH_GATE_H
#define GATEBENCH_GATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gatebench_nl.h"

/* Forward declaration - defined in gatebench.h */
struct gate_shape;

/* Netlink message types for tc actions */
#define RTM_NEWACTION 48
#define RTM_DELACTION 49
#define RTM_GETACTION 50

/* Action types */
#define TC_ACT_PIPE 3    /* Continue processing (used in tc_gate.action) */

/* Netlink attribute types for gate action - from linux/tc_act/tc_gate.h */
#define TCA_GATE_PARMS          2
#define TCA_GATE_PAD            3
#define TCA_GATE_PRIORITY       4
#define TCA_GATE_ENTRY_LIST     6
#define TCA_GATE_BASE_TIME      7
#define TCA_GATE_CYCLE_TIME     8
#define TCA_GATE_CYCLE_TIME_EXT 9
#define TCA_GATE_FLAGS          10
#define TCA_GATE_CLOCKID        11

/* Gate entry attribute types */
#define TCA_GATE_ENTRY_UNSPEC     0
#define TCA_GATE_ENTRY_INDEX      1
#define TCA_GATE_ENTRY_GATE       2
#define TCA_GATE_ENTRY_INTERVAL   3
#define TCA_GATE_ENTRY_IPV        4
#define TCA_GATE_ENTRY_MAX_OCTETS 5

/* TCA_GATE_ONE_ENTRY for nested entry attributes */
#define TCA_GATE_ONE_ENTRY       1

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
                         uint32_t flags);

/* Build RTM_DELACTION message */
int build_gate_delaction(struct gb_nl_msg *msg, uint32_t index);

/* Add a gate entry to message */
int add_gate_entry(struct gb_nl_msg *msg,
                   const struct gate_entry *entry);

#endif /* GATEBENCH_GATE_H */