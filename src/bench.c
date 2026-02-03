/* src/bench.c
 * Core benchmarking logic and measurement loop.
 */

#include "../include/gatebench.h"
#include "../include/gatebench_bench.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"
#include "../include/gatebench_stats.h"
#include "../include/gatebench_util.h"
#include "bench_internal.h"

#include <errno.h>
#include <libmnl/libmnl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int gb_fill_entries(struct gate_entry* entries, uint32_t n, uint64_t interval_ns) {
    if (!entries || n == 0)
        return 0;

    if (interval_ns > UINT32_MAX || interval_ns == 0)
        return -ERANGE;

    for (uint32_t i = 0; i < n; i++) {
        bool guard_slot = (n >= 10u) && ((i + 1u) % 10u == 0u);

        entries[i].index = i;
        entries[i].interval = (uint32_t)interval_ns;

        if (guard_slot) {
            entries[i].gate_state = false;
            entries[i].ipv = -1;
            entries[i].maxoctets = -1;
            continue;
        }

        entries[i].gate_state = true;

        /* tc gate "ipv" attribute is internal priority; use it to model class windows. */
        if ((i % 2u) == 0u) {
            entries[i].ipv = 7;
            entries[i].maxoctets = 8192;
        }
        else {
            entries[i].ipv = 0;
            entries[i].maxoctets = 32768;
        }
    }

    return 0;
}

static void stats_add_sample(struct gb_stats* stats, const struct gb_config* cfg, uint32_t i, uint64_t latency_ns) {
    if (!cfg->sample_mode) {
        gb_stats_add(stats, latency_ns);
        return;
    }

    if (cfg->sample_every == 0)
        return;

    if ((i % cfg->sample_every) == 0)
        gb_stats_add(stats, latency_ns);
}

static int benchmark_single_run(struct gb_nl_sock* sock, const struct gb_config* cfg, struct gb_run_result* result) {
    struct gb_nl_msg* create_msg = NULL;
    struct gb_nl_msg* replace_msg = NULL;
    struct gb_nl_msg* del_msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry* entries = NULL;
    struct gb_stats stats;
    size_t create_cap, replace_cap, del_cap;
    uint64_t start_ns, end_ns;
    int ret;

    if (!sock || !cfg || !result)
        return -EINVAL;

    memset(result, 0, sizeof(*result));

    ret = gb_stats_init(&stats, cfg->iters * 2);
    if (ret < 0)
        return ret;

    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!resp) {
        ret = -ENOMEM;
        goto out;
    }

    memset(&shape, 0, sizeof(shape));
    shape.clockid = cfg->clockid;
    shape.base_time = cfg->base_time;
    shape.cycle_time = cfg->cycle_time;
    shape.cycle_time_ext = cfg->cycle_time_ext;
    shape.interval_ns = cfg->interval_ns;
    shape.entries = cfg->entries;

    if (cfg->entries > 0) {
        entries = malloc((size_t)cfg->entries * sizeof(*entries));
        if (!entries) {
            ret = -ENOMEM;
            goto out;
        }

        ret = gb_fill_entries(entries, cfg->entries, cfg->interval_ns);
        if (ret < 0)
            goto out;
    }

    create_cap = gate_msg_capacity(cfg->entries, 0);
    replace_cap = gate_msg_capacity(cfg->entries, 0);
    del_cap = 1024;

    create_msg = gb_nl_msg_alloc(create_cap);
    replace_msg = gb_nl_msg_alloc(replace_cap);
    del_msg = gb_nl_msg_alloc(del_cap);
    if (!create_msg || !replace_msg || !del_msg) {
        ret = -ENOMEM;
        goto out;
    }

    ret = build_gate_newaction(create_msg, cfg->index, &shape, entries, cfg->entries, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0)
        goto out;

    ret = build_gate_newaction(replace_msg, cfg->index, &shape, entries, cfg->entries, NLM_F_CREATE | NLM_F_REPLACE, 0,
                               -1);
    if (ret < 0)
        goto out;

    ret = build_gate_delaction(del_msg, cfg->index);
    if (ret < 0)
        goto out;

    result->create_len = (uint32_t)create_msg->len;
    result->replace_len = (uint32_t)replace_msg->len;
    result->del_len = (uint32_t)del_msg->len;

    for (uint32_t i = 0; i < cfg->warmup; i++) {
        ret = gb_nl_send_recv(sock, create_msg, resp, cfg->timeout_ms);
        if (ret < 0 && ret != -EEXIST)
            goto out;

        ret = gb_nl_send_recv(sock, replace_msg, resp, cfg->timeout_ms);
        if (ret < 0)
            goto out;
    }

    ret = gb_nl_send_recv(sock, del_msg, resp, cfg->timeout_ms);
    if (ret < 0 && ret != -ENOENT)
        goto out;

    ret = gb_util_ns_now(&start_ns, CLOCK_MONOTONIC_RAW);
    if (ret < 0)
        goto out;

    for (uint32_t i = 0; i < cfg->iters; i++) {
        uint64_t a, b;

        ret = gb_util_ns_now(&a, CLOCK_MONOTONIC_RAW);
        if (ret < 0)
            goto out;
        ret = gb_nl_send_recv(sock, create_msg, resp, cfg->timeout_ms);
        if (ret < 0 && ret != -EEXIST)
            goto out;
        ret = gb_util_ns_now(&b, CLOCK_MONOTONIC_RAW);
        if (ret < 0)
            goto out;

        stats_add_sample(&stats, cfg, i, b - a);

        ret = gb_util_ns_now(&a, CLOCK_MONOTONIC_RAW);
        if (ret < 0)
            goto out;
        ret = gb_nl_send_recv(sock, replace_msg, resp, cfg->timeout_ms);
        if (ret < 0)
            goto out;
        ret = gb_util_ns_now(&b, CLOCK_MONOTONIC_RAW);
        if (ret < 0)
            goto out;

        stats_add_sample(&stats, cfg, i, b - a);
    }

    ret = gb_util_ns_now(&end_ns, CLOCK_MONOTONIC_RAW);
    if (ret < 0)
        goto out;

    ret = gb_nl_send_recv(sock, del_msg, resp, cfg->timeout_ms);
    if (ret < 0 && ret != -ENOENT)
        ret = 0;

    result->secs = (double)(end_ns - start_ns) / 1e9;
    if (result->secs > 0.0)
        result->ops_per_sec = ((double)cfg->iters * 2.0) / result->secs;

    ret = gb_stats_calculate(&stats, &result->min_ns, &result->max_ns, &result->mean_ns, &result->stddev_ns,
                             &result->p50_ns, &result->p95_ns, &result->p99_ns, &result->p999_ns);
    if (ret < 0)
        goto out;

    if (cfg->sample_mode) {
        result->sample_count = (uint32_t)stats.count;
        if (stats.count > 0) {
            result->samples = malloc((size_t)stats.count * sizeof(uint64_t));
            if (result->samples)
                memcpy(result->samples, stats.values, (size_t)stats.count * sizeof(uint64_t));
        }
    }

    ret = 0;

out:
    gb_stats_free(&stats);
    free(entries);

    if (create_msg)
        gb_nl_msg_free(create_msg);
    if (replace_msg)
        gb_nl_msg_free(replace_msg);
    if (del_msg)
        gb_nl_msg_free(del_msg);
    if (resp)
        gb_nl_msg_free(resp);

    return ret;
}

int gb_bench_run(const struct gb_config* cfg, struct gb_summary* summary) {
    struct gb_nl_sock* sock = NULL;
    struct gb_run_result* runs = NULL;
    double* ops_array = NULL;
    uint64_t* p50_array = NULL;
    uint64_t* p95_array = NULL;
    uint64_t* p99_array = NULL;
    uint64_t* p999_array = NULL;
    double sum = 0.0, sum_sq = 0.0, mean;
    int ret;

    if (!cfg || !summary)
        return -EINVAL;

    memset(summary, 0, sizeof(*summary));

    ret = gb_nl_open(&sock);
    if (ret < 0)
        return ret;

    runs = calloc(cfg->runs, sizeof(*runs));
    if (!runs) {
        ret = -ENOMEM;
        goto out;
    }

    for (uint32_t i = 0; i < cfg->runs; i++) {
        if (!cfg->json) {
            printf("Run %u/%u... ", i + 1, cfg->runs);
            fflush(stdout);
        }

        ret = benchmark_single_run(sock, cfg, &runs[i]);
        if (ret < 0) {
            if (!cfg->json)
                printf("failed: %s\n", strerror(-ret));
            goto out;
        }

        if (!cfg->json)
            printf("done (%.1f ops/sec)\n", runs[i].ops_per_sec);
    }

    summary->runs = runs;
    summary->run_count = cfg->runs;

    ops_array = malloc((size_t)cfg->runs * sizeof(*ops_array));
    if (!ops_array) {
        ret = -ENOMEM;
        goto out;
    }
    for (uint32_t i = 0; i < cfg->runs; i++)
        ops_array[i] = runs[i].ops_per_sec;
    ret = gb_stats_median_double(ops_array, cfg->runs, &summary->median_ops_per_sec);
    free(ops_array);
    ops_array = NULL;
    if (ret < 0)
        goto out;

    summary->min_ops_per_sec = runs[0].ops_per_sec;
    summary->max_ops_per_sec = runs[0].ops_per_sec;
    for (uint32_t i = 1; i < cfg->runs; i++) {
        if (runs[i].ops_per_sec < summary->min_ops_per_sec)
            summary->min_ops_per_sec = runs[i].ops_per_sec;
        if (runs[i].ops_per_sec > summary->max_ops_per_sec)
            summary->max_ops_per_sec = runs[i].ops_per_sec;
    }

    p50_array = malloc((size_t)cfg->runs * sizeof(*p50_array));
    p95_array = malloc((size_t)cfg->runs * sizeof(*p95_array));
    p99_array = malloc((size_t)cfg->runs * sizeof(*p99_array));
    p999_array = malloc((size_t)cfg->runs * sizeof(*p999_array));

    if (!p50_array || !p95_array || !p99_array || !p999_array) {
        ret = -ENOMEM;
        goto out;
    }

    for (uint32_t i = 0; i < cfg->runs; i++) {
        p50_array[i] = runs[i].p50_ns;
        p95_array[i] = runs[i].p95_ns;
        p99_array[i] = runs[i].p99_ns;
        p999_array[i] = runs[i].p999_ns;
    }

    ret = gb_stats_median_uint64(p50_array, cfg->runs, &summary->median_p50_ns);
    if (ret < 0)
        goto out;
    ret = gb_stats_median_uint64(p95_array, cfg->runs, &summary->median_p95_ns);
    if (ret < 0)
        goto out;
    ret = gb_stats_median_uint64(p99_array, cfg->runs, &summary->median_p99_ns);
    if (ret < 0)
        goto out;
    ret = gb_stats_median_uint64(p999_array, cfg->runs, &summary->median_p999_ns);
    if (ret < 0)
        goto out;

    free(p50_array);
    free(p95_array);
    free(p99_array);
    free(p999_array);
    p50_array = NULL;
    p95_array = NULL;
    p99_array = NULL;
    p999_array = NULL;

    for (uint32_t i = 0; i < cfg->runs; i++) {
        sum += runs[i].ops_per_sec;
        sum_sq += runs[i].ops_per_sec * runs[i].ops_per_sec;
    }
    mean = sum / (double)cfg->runs;
    summary->stddev_ops_per_sec = sqrt((sum_sq / (double)cfg->runs) - (mean * mean));

    ret = 0;

out:
    free(ops_array);
    free(p50_array);
    free(p95_array);
    free(p99_array);
    free(p999_array);

    if (ret < 0) {
        if (runs) {
            for (uint32_t i = 0; i < cfg->runs; i++)
                gb_run_result_free(&runs[i]);
            free(runs);
        }
    }

    if (sock)
        gb_nl_close(sock);

    return ret;
}
