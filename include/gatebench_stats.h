/* include/gatebench_stats.h
 * Public API for statistics.
 */
#ifndef GATEBENCH_STATS_H
#define GATEBENCH_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Statistics calculation context */
struct gb_stats {
    uint64_t* values;
    size_t count;
    size_t capacity;
    bool sorted;
};

/* Initialize statistics context */
int gb_stats_init(struct gb_stats* stats, size_t initial_capacity);

/* Free statistics context */
void gb_stats_free(struct gb_stats* stats);

/* Add value to statistics */
int gb_stats_add(struct gb_stats* stats, uint64_t value);

/* Sort values (if not already sorted) */
void gb_stats_sort(struct gb_stats* stats);

/* Calculate percentile (0.0 to 1.0). */
int gb_stats_percentile(struct gb_stats* stats, double p, uint64_t* out);

/* Calculate mean. */
int gb_stats_mean(struct gb_stats* stats, double* out);

/* Calculate standard deviation. */
int gb_stats_stddev(struct gb_stats* stats, double* out);

/* Calculate minimum value. */
int gb_stats_min(struct gb_stats* stats, uint64_t* out);

/* Calculate maximum value. */
int gb_stats_max(struct gb_stats* stats, uint64_t* out);

/* Calculate all statistics at once */
int gb_stats_calculate(struct gb_stats* stats,
                       uint64_t* min,
                       uint64_t* max,
                       double* mean,
                       double* stddev,
                       uint64_t* p50,
                       uint64_t* p95,
                       uint64_t* p99,
                       uint64_t* p999);

/* Calculate median of an array of doubles. */
int gb_stats_median_double(const double* values, size_t count, double* out);

/* Calculate median of an array of uint64_t. */
int gb_stats_median_uint64(const uint64_t* values, size_t count, uint64_t* out);

#endif /* GATEBENCH_STATS_H */
