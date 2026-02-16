/* include/gatebench_race.h
 * Public API for race mode workload.
 */
#ifndef GATEBENCH_RACE_H
#define GATEBENCH_RACE_H

#include "gatebench.h"

#define GB_RACE_THREAD_COUNT 8u

struct gb_race_worker_summary {
    int cpu;
    uint64_t ops;
    uint64_t errors;
};

struct gb_race_sync_worker_summary {
    int cpu;
    uint64_t ops;
};

struct gb_race_summary {
    bool completed;
    uint32_t duration_seconds;
    int cpu_count;
    struct gb_race_worker_summary replace;
    struct gb_race_worker_summary dump;
    struct gb_race_worker_summary get;
    struct gb_race_worker_summary traffic;
    struct gb_race_sync_worker_summary traffic_sync;
    struct gb_race_worker_summary basetime;
    struct gb_race_worker_summary delete_worker;
    struct gb_race_worker_summary invalid;
};

/* Run race mode workload */
int gb_race_run(const struct gb_config* cfg);
int gb_race_run_with_summary(const struct gb_config* cfg, struct gb_race_summary* summary);

#endif /* GATEBENCH_RACE_H */
