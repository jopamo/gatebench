/* src/stats.c
 * Statistical analysis of benchmark results.
 */
#include "../include/gatebench_stats.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int compare_u64(const void* pa, const void* pb) {
    const uint64_t a = *(const uint64_t*)pa;
    const uint64_t b = *(const uint64_t*)pb;

    return (a > b) - (a < b);
}

static int compare_double(const void* pa, const void* pb) {
    const double a = *(const double*)pa;
    const double b = *(const double*)pb;

    return (a > b) - (a < b);
}

int gb_stats_init(struct gb_stats* stats, size_t initial_capacity) {
    if (!stats)
        return -EINVAL;

    memset(stats, 0, sizeof(*stats));

    if (initial_capacity == 0)
        return 0;

    stats->values = malloc(initial_capacity * sizeof(*stats->values));
    if (!stats->values)
        return -ENOMEM;

    stats->capacity = initial_capacity;
    return 0;
}

void gb_stats_free(struct gb_stats* stats) {
    if (!stats)
        return;

    free(stats->values);
    memset(stats, 0, sizeof(*stats));
}

int gb_stats_add(struct gb_stats* stats, uint64_t value) {
    if (!stats)
        return -EINVAL;

    if (stats->count >= stats->capacity) {
        size_t new_capacity = stats->capacity ? stats->capacity * 2 : 16;
        uint64_t* new_values;

        new_values = realloc(stats->values, new_capacity * sizeof(*new_values));
        if (!new_values)
            return -ENOMEM;

        stats->values = new_values;
        stats->capacity = new_capacity;
    }

    stats->values[stats->count++] = value;
    stats->sorted = false;
    return 0;
}

void gb_stats_sort(struct gb_stats* stats) {
    if (!stats || !stats->values || stats->count == 0 || stats->sorted)
        return;

    qsort(stats->values, stats->count, sizeof(*stats->values), compare_u64);
    stats->sorted = true;
}

int gb_stats_percentile(struct gb_stats* stats, double p, uint64_t* out) {
    if (!stats || !stats->values || stats->count == 0 || !out)
        return -EINVAL;

    if (p < 0.0 || p > 1.0)
        return -EINVAL;

    if (!stats->sorted)
        gb_stats_sort(stats);

    if (p <= 0.0) {
        *out = stats->values[0];
        return 0;
    }

    if (p >= 1.0) {
        *out = stats->values[stats->count - 1];
        return 0;
    }

    {
        const double idx = p * (double)(stats->count - 1);
        const double floor_idx = floor(idx);
        const double ceil_idx = ceil(idx);
        const size_t lo = (size_t)floor_idx;
        const size_t hi = (size_t)ceil_idx;

        if (lo == hi) {
            *out = stats->values[lo];
            return 0;
        }

        {
            const double w = idx - (double)lo;
            const double a = (double)stats->values[lo];
            const double b = (double)stats->values[hi];

            *out = (uint64_t)(a + w * (b - a));
            return 0;
        }
    }
}

int gb_stats_mean(struct gb_stats* stats, double* out) {
    if (!stats || !stats->values || stats->count == 0 || !out)
        return -EINVAL;

    {
        double sum = 0.0;

        for (size_t i = 0; i < stats->count; i++)
            sum += (double)stats->values[i];

        *out = sum / (double)stats->count;
        return 0;
    }
}

int gb_stats_stddev(struct gb_stats* stats, double* out) {
    double mean = 0.0;
    double sum_sq = 0.0;
    int ret;

    if (!stats || !stats->values || stats->count == 0 || !out)
        return -EINVAL;

    if (stats->count < 2) {
        *out = 0.0;
        return 0;
    }

    ret = gb_stats_mean(stats, &mean);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < stats->count; i++) {
        const double diff = (double)stats->values[i] - mean;
        sum_sq += diff * diff;
    }

    *out = sqrt(sum_sq / (double)(stats->count - 1));
    return 0;
}

int gb_stats_min(struct gb_stats* stats, uint64_t* out) {
    if (!stats || !stats->values || stats->count == 0 || !out)
        return -EINVAL;

    if (!stats->sorted)
        gb_stats_sort(stats);

    *out = stats->values[0];
    return 0;
}

int gb_stats_max(struct gb_stats* stats, uint64_t* out) {
    if (!stats || !stats->values || stats->count == 0 || !out)
        return -EINVAL;

    if (!stats->sorted)
        gb_stats_sort(stats);

    *out = stats->values[stats->count - 1];
    return 0;
}

int gb_stats_calculate(struct gb_stats* stats,
                       uint64_t* min,
                       uint64_t* max,
                       double* mean,
                       double* stddev,
                       uint64_t* p50,
                       uint64_t* p95,
                       uint64_t* p99,
                       uint64_t* p999) {
    int ret;

    if (!stats || !stats->values || stats->count == 0)
        return -EINVAL;

    if (!stats->sorted)
        gb_stats_sort(stats);

    if (min)
        *min = stats->values[0];
    if (max)
        *max = stats->values[stats->count - 1];
    if (mean) {
        ret = gb_stats_mean(stats, mean);
        if (ret < 0)
            return ret;
    }
    if (stddev) {
        ret = gb_stats_stddev(stats, stddev);
        if (ret < 0)
            return ret;
    }
    if (p50) {
        ret = gb_stats_percentile(stats, 0.50, p50);
        if (ret < 0)
            return ret;
    }
    if (p95) {
        ret = gb_stats_percentile(stats, 0.95, p95);
        if (ret < 0)
            return ret;
    }
    if (p99) {
        ret = gb_stats_percentile(stats, 0.99, p99);
        if (ret < 0)
            return ret;
    }
    if (p999) {
        ret = gb_stats_percentile(stats, 0.999, p999);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int gb_stats_median_double(const double* values, size_t count, double* out) {
    double* copy;

    if (!values || count == 0 || !out)
        return -EINVAL;

    copy = malloc(count * sizeof(*copy));
    if (!copy)
        return -ENOMEM;

    memcpy(copy, values, count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), compare_double);

    if ((count & 1u) == 0u)
        *out = (copy[count / 2 - 1] + copy[count / 2]) / 2.0;
    else
        *out = copy[count / 2];

    free(copy);
    return 0;
}

int gb_stats_median_uint64(const uint64_t* values, size_t count, uint64_t* out) {
    uint64_t* copy;

    if (!values || count == 0 || !out)
        return -EINVAL;

    copy = malloc(count * sizeof(*copy));
    if (!copy)
        return -ENOMEM;

    memcpy(copy, values, count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), compare_u64);

    if ((count & 1u) == 0u)
        *out = (copy[count / 2 - 1] + copy[count / 2]) / 2;
    else
        *out = copy[count / 2];

    free(copy);
    return 0;
}
