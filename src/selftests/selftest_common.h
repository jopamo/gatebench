#ifndef GATEBENCH_SELFTEST_COMMON_H
#define GATEBENCH_SELFTEST_COMMON_H

#include "../../include/gatebench.h"
#include "../../include/gatebench_gate.h"
#include "../../include/gatebench_nl.h"
#include <libmnl/libmnl.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define GB_SELFTEST_TIMEOUT_MS 1000
#define GB_SELFTEST_DEFAULT_INTERVAL_NS 1000000

typedef int (*gb_selftest_fn)(struct gb_nl_sock* sock, uint32_t base_index);

struct gb_selftest_case {
    const char* name;
    gb_selftest_fn func;
    int expected_err;
};

void gb_selftest_shape_default(struct gate_shape* shape, uint32_t entries);
void gb_selftest_entry_default(struct gate_entry* entry);
int gb_selftest_alloc_msgs(struct gb_nl_msg** msg, struct gb_nl_msg** resp, size_t capacity);
void gb_selftest_free_msgs(struct gb_nl_msg* msg, struct gb_nl_msg* resp);
void gb_selftest_cleanup_gate(struct gb_nl_sock* sock, struct gb_nl_msg* msg, struct gb_nl_msg* resp, uint32_t index);
void gb_selftest_set_verbose(bool verbose);
void gb_selftest_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* GATEBENCH_SELFTEST_COMMON_H */
