/* include/gatebench_selftest.h
 * Public API for self-tests.
 */
#ifndef GATEBENCH_SELFTEST_H
#define GATEBENCH_SELFTEST_H

#include "gatebench.h"

/* Run selftests. Returns 0 on full pass, >0 for soft failures, <0 on hard failures. */
int gb_selftest_run(struct gb_config* cfg);

#endif /* GATEBENCH_SELFTEST_H */
