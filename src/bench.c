#include "../include/gatebench.h"
#include "../include/gatebench_bench.h"
#include "../include/gatebench_nl.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_stats.h"
#include "../include/gatebench_util.h"
#include <libmnl/libmnl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>

static int benchmark_single_run(struct gb_nl_sock *sock,
                               const struct gb_config *cfg,
                               struct gb_run_result *result) {
    struct gb_nl_msg *create_msg = NULL;
    struct gb_nl_msg *replace_msg = NULL;
    struct gb_nl_msg *del_msg = NULL;
    struct gb_nl_msg *resp = NULL;
    struct gate_shape shape;
    struct gate_entry *entries = NULL;
    struct gb_stats stats;
    uint64_t start_ns, end_ns, latency_ns;
    uint32_t i;
    size_t create_cap, replace_cap, del_cap;
    int ret = 0;
    
    /* Initialize statistics */
    ret = gb_stats_init(&stats, cfg->iters);
    if (ret < 0) {
        return ret;
    }
    
    /* Allocate response buffer */
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!resp) {
        ret = -ENOMEM;
        goto out;
    }
    
    /* Prepare gate shape */
    shape.clockid = cfg->clockid;
    shape.base_time = cfg->base_time;
    shape.cycle_time = cfg->cycle_time;
    shape.interval_ns = cfg->interval_ns;
    shape.entries = cfg->entries;
    
    /* Prepare gate entries */
    if (cfg->entries > 0) {
        entries = malloc(cfg->entries * sizeof(struct gate_entry));
        if (!entries) {
            ret = -ENOMEM;
            goto out;
        }
        
        for (i = 0; i < cfg->entries; i++) {
            entries[i].gate_state = true; /* Default to open gate */
            entries[i].interval = (uint32_t)cfg->interval_ns;
            entries[i].ipv = -1; /* Any IP version (-1 for wildcard) */
            entries[i].maxoctets = -1; /* No limit (-1 for unlimited) */
        }
    }
    
    /* Calculate message capacities */
    create_cap = gate_msg_capacity(cfg->entries, 0);
    replace_cap = gate_msg_capacity(cfg->entries, 0);
    del_cap = 1024; /* Small message for delete */
    
    /* Allocate messages */
    create_msg = gb_nl_msg_alloc(create_cap);
    replace_msg = gb_nl_msg_alloc(replace_cap);
    del_msg = gb_nl_msg_alloc(del_cap);
    
    if (!create_msg || !replace_msg || !del_msg) {
        ret = -ENOMEM;
        goto out;
    }
    
    /* Build messages */
    ret = build_gate_newaction(create_msg, cfg->index, &shape,
                               entries, cfg->entries,
                               NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        goto out;
    }
    
    ret = build_gate_newaction(replace_msg, cfg->index, &shape,
                               entries, cfg->entries,
                               NLM_F_CREATE | NLM_F_REPLACE, 0, -1);
    if (ret < 0) {
        goto out;
    }
    
    ret = build_gate_delaction(del_msg, cfg->index);
    if (ret < 0) {
        goto out;
    }
    
    /* Store message sizes in result */
    result->create_len = (uint32_t)create_msg->len;
    result->replace_len = (uint32_t)replace_msg->len;
    result->del_len = (uint32_t)del_msg->len;
    
    /* Warmup phase */
    for (i = 0; i < cfg->warmup; i++) {
        /* Create */
        ret = gb_nl_send_recv(sock, create_msg, resp, cfg->timeout_ms);
        if (ret < 0 && ret != -EEXIST) {
            /* Ignore EEXIST (already exists) during warmup */
            goto out;
        }
        
        /* Replace */
        ret = gb_nl_send_recv(sock, replace_msg, resp, cfg->timeout_ms);
        if (ret < 0) {
            goto out;
        }
    }
    
    /* Delete after warmup */
    ret = gb_nl_send_recv(sock, del_msg, resp, cfg->timeout_ms);
    if (ret < 0 && ret != -ENOENT) {
        /* Ignore ENOENT (not found) */
        goto out;
    }
    
    /* Measurement phase */
    start_ns = gb_util_ns_now(CLOCK_MONOTONIC_RAW);
    
    for (i = 0; i < cfg->iters; i++) {
        uint64_t iter_start, iter_end;
        
        /* Measure create latency */
        iter_start = gb_util_ns_now(CLOCK_MONOTONIC_RAW);
        ret = gb_nl_send_recv(sock, create_msg, resp, cfg->timeout_ms);
        iter_end = gb_util_ns_now(CLOCK_MONOTONIC_RAW);
        
        if (ret < 0 && ret != -EEXIST) {
            goto out;
        }
        
        latency_ns = iter_end - iter_start;
        
        /* Store sample if sampling is enabled */
        if (cfg->sample_mode && (i % cfg->sample_every) == 0) {
            gb_stats_add(&stats, latency_ns);
        } else if (!cfg->sample_mode) {
            /* Store all samples if not sampling */
            gb_stats_add(&stats, latency_ns);
        }
        
        /* Measure replace latency */
        iter_start = gb_util_ns_now(CLOCK_MONOTONIC_RAW);
        ret = gb_nl_send_recv(sock, replace_msg, resp, cfg->timeout_ms);
        iter_end = gb_util_ns_now(CLOCK_MONOTONIC_RAW);
        
        if (ret < 0) {
            goto out;
        }
        
        latency_ns = iter_end - iter_start;
        
        /* Store sample if sampling is enabled */
        if (cfg->sample_mode && (i % cfg->sample_every) == 0) {
            gb_stats_add(&stats, latency_ns);
        } else if (!cfg->sample_mode) {
            /* Store all samples if not sampling */
            gb_stats_add(&stats, latency_ns);
        }
    }
    
    end_ns = gb_util_ns_now(CLOCK_MONOTONIC_RAW);
    
    /* Cleanup - delete the gate */
    ret = gb_nl_send_recv(sock, del_msg, resp, cfg->timeout_ms);
    if (ret < 0 && ret != -ENOENT) {
        /* Ignore ENOENT */
    }
    
    /* Calculate statistics */
    result->secs = (double)(end_ns - start_ns) / 1e9;
    result->ops_per_sec = (cfg->iters * 2) / result->secs; /* create + replace */
    
    gb_stats_calculate(&stats,
                      &result->min_ns, &result->max_ns,
                      &result->mean_ns, &result->stddev_ns,
                      &result->p50_ns, &result->p95_ns,
                      &result->p99_ns, &result->p999_ns);
    
    /* Store samples if we collected them */
    if (cfg->sample_mode) {
        result->sample_count = (uint32_t)stats.count;
        if (stats.count > 0) {
            result->samples = malloc(stats.count * sizeof(uint64_t));
            if (result->samples) {
                memcpy(result->samples, stats.values, 
                      stats.count * sizeof(uint64_t));
            }
        }
    }
    
out:
    /* Cleanup */
    gb_stats_free(&stats);
    if (entries) free(entries);
    if (create_msg) gb_nl_msg_free(create_msg);
    if (replace_msg) gb_nl_msg_free(replace_msg);
    if (del_msg) gb_nl_msg_free(del_msg);
    if (resp) gb_nl_msg_free(resp);
    
    return ret;
}

int gb_bench_run(const struct gb_config *cfg, struct gb_summary *summary) {
    struct gb_nl_sock *sock = NULL;
    struct gb_run_result *runs = NULL;
    double *ops_array = NULL;
    uint64_t *p50_array = NULL;
    uint64_t *p95_array = NULL;
    uint64_t *p99_array = NULL;
    uint64_t *p999_array = NULL;
    double sum, sum_sq, mean;
    uint32_t i;
    int ret = 0;
    
    if (!cfg || !summary) {
        return -EINVAL;
    }
    
    /* Open netlink socket */
    ret = gb_nl_open(&sock);
    if (ret < 0) {
        return ret;
    }
    
    /* Allocate run results array */
    runs = malloc(cfg->runs * sizeof(struct gb_run_result));
    if (!runs) {
        ret = -ENOMEM;
        goto out;
    }
    
    memset(runs, 0, cfg->runs * sizeof(struct gb_run_result));
    
    /* Run benchmark multiple times */
    for (i = 0; i < cfg->runs; i++) {
        if (!cfg->json) {
            printf("Run %u/%u... ", i + 1, cfg->runs);
            fflush(stdout);
        }
        
        ret = benchmark_single_run(sock, cfg, &runs[i]);
        if (ret < 0) {
            if (!cfg->json) {
                printf("failed: %s\n", strerror(-ret));
            }
            goto out;
        }
        
        if (!cfg->json) {
            printf("done (%.1f ops/sec)\n", runs[i].ops_per_sec);
        }
    }
    
    /* Calculate summary statistics */
    summary->runs = runs;
    summary->run_count = cfg->runs;
    
    /* Calculate median ops/sec */
    ops_array = malloc(cfg->runs * sizeof(double));
    if (ops_array) {
        for (i = 0; i < cfg->runs; i++) {
            ops_array[i] = runs[i].ops_per_sec;
        }
        summary->median_ops_per_sec = gb_stats_median_double(ops_array, cfg->runs);
        free(ops_array);
    }
    
    /* Calculate min/max ops/sec */
    summary->min_ops_per_sec = runs[0].ops_per_sec;
    summary->max_ops_per_sec = runs[0].ops_per_sec;
    for (i = 1; i < cfg->runs; i++) {
        if (runs[i].ops_per_sec < summary->min_ops_per_sec) {
            summary->min_ops_per_sec = runs[i].ops_per_sec;
        }
        if (runs[i].ops_per_sec > summary->max_ops_per_sec) {
            summary->max_ops_per_sec = runs[i].ops_per_sec;
        }
    }
    
    /* Calculate median latencies */
    p50_array = malloc(cfg->runs * sizeof(uint64_t));
    p95_array = malloc(cfg->runs * sizeof(uint64_t));
    p99_array = malloc(cfg->runs * sizeof(uint64_t));
    p999_array = malloc(cfg->runs * sizeof(uint64_t));
    
    if (p50_array && p95_array && p99_array && p999_array) {
        for (i = 0; i < cfg->runs; i++) {
            p50_array[i] = runs[i].p50_ns;
            p95_array[i] = runs[i].p95_ns;
            p99_array[i] = runs[i].p99_ns;
            p999_array[i] = runs[i].p999_ns;
        }
        
        summary->median_p50_ns = gb_stats_median_uint64(p50_array, cfg->runs);
        summary->median_p95_ns = gb_stats_median_uint64(p95_array, cfg->runs);
        summary->median_p99_ns = gb_stats_median_uint64(p99_array, cfg->runs);
        summary->median_p999_ns = gb_stats_median_uint64(p999_array, cfg->runs);
        
        free(p50_array);
        free(p95_array);
        free(p99_array);
        free(p999_array);
    }
    
    /* Calculate stddev of ops/sec */
    sum = 0.0;
    sum_sq = 0.0;
    for (i = 0; i < cfg->runs; i++) {
        sum += runs[i].ops_per_sec;
        sum_sq += runs[i].ops_per_sec * runs[i].ops_per_sec;
    }
    mean = sum / cfg->runs;
    summary->stddev_ops_per_sec = sqrt((sum_sq / cfg->runs) - (mean * mean));
    
out:
    if (ret < 0) {
        /* Free runs on error */
        if (runs) {
            for (i = 0; i < cfg->runs; i++) {
                gb_run_result_free(&runs[i]);
            }
            free(runs);
        }
    }
    
    if (sock) {
        gb_nl_close(sock);
    }
    
    return ret;
}
