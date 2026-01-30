#ifndef GATEBENCH_BENCH_H
#define GATEBENCH_BENCH_H

#include "gatebench.h"

/* Run benchmark */
int gb_bench_run(const struct gb_config *cfg, struct gb_summary *summary);

/* Free run result */
void gb_run_result_free(struct gb_run_result *result);

/* Free summary */
void gb_summary_free(struct gb_summary *summary);

#endif /* GATEBENCH_BENCH_H */