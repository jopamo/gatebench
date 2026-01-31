#ifndef GATEBENCH_BENCH_INTERNAL_H
#define GATEBENCH_BENCH_INTERNAL_H

#include <stdint.h>
#include "gatebench_gate.h"

int gb_fill_entries(struct gate_entry* entries, uint32_t n, uint64_t interval_ns);

#endif /* GATEBENCH_BENCH_INTERNAL_H */
