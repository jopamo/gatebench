/* include/gatebench_race.h
 * Public API for race mode workload.
 */
#ifndef GATEBENCH_RACE_H
#define GATEBENCH_RACE_H

#include "gatebench.h"

/* Run race mode workload */
int gb_race_run(const struct gb_config* cfg);

#endif /* GATEBENCH_RACE_H */
