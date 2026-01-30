#ifndef GATEBENCH_H
#define GATEBENCH_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Core configuration structure */
struct gb_config {
    /* Benchmark parameters */
    uint32_t iters;       /* Number of iterations per run */
    uint32_t warmup;      /* Warmup iterations */
    uint32_t runs;        /* Number of runs */
    uint32_t entries;     /* Number of gate entries */
    uint64_t interval_ns; /* Gate interval in nanoseconds */
    uint32_t index;       /* Starting index for gate actions */

    /* System configuration */
    int cpu;        /* CPU to pin to (-1 for no pinning) */
    int timeout_ms; /* Netlink timeout in milliseconds */

    /* Mode flags */
    bool selftest;         /* Run selftests before benchmark */
    bool json;             /* Output JSON format */
    bool sample_mode;      /* Sample every N iterations */
    uint32_t sample_every; /* Sampling interval */

    /* Gate shape parameters */
    uint32_t clockid;    /* Clock ID (CLOCK_TAI, CLOCK_MONOTONIC, etc.) */
    uint64_t base_time;  /* Base time for gate schedule */
    uint64_t cycle_time; /* Cycle time for gate schedule */
};

/* Gate shape structure */
struct gate_shape {
    uint32_t clockid;
    uint64_t base_time;
    uint64_t cycle_time;
    uint64_t interval_ns;
    uint32_t entries;
};

/* Single run results */
struct gb_run_result {
    double secs;        /* Total time in seconds */
    double ops_per_sec; /* Operations per second */

    /* Latency percentiles in nanoseconds */
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t p999_ns;
    uint64_t min_ns;
    uint64_t max_ns;

    /* Statistical measures */
    double mean_ns;
    double stddev_ns;

    /* Message sizes */
    uint32_t create_len;
    uint32_t replace_len;
    uint32_t del_len;

    /* Raw latency samples (if sampling enabled) */
    uint64_t* samples;
    uint32_t sample_count;
};

/* Summary across multiple runs */
struct gb_summary {
    struct gb_run_result* runs;
    uint32_t run_count;

    /* Aggregated statistics */
    double median_ops_per_sec;
    double min_ops_per_sec;
    double max_ops_per_sec;
    double stddev_ops_per_sec;

    /* Latency statistics across runs */
    uint64_t median_p50_ns;
    uint64_t median_p95_ns;
    uint64_t median_p99_ns;
    uint64_t median_p999_ns;
};

/* Function prototypes */
void gb_config_init(struct gb_config* cfg);
void gb_config_print(const struct gb_config* cfg);
void gb_run_result_free(struct gb_run_result* result);
void gb_summary_free(struct gb_summary* summary);

#endif /* GATEBENCH_H */