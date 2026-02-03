/* src/race.c
 * Race mode workload generation.
 */
#include "../include/gatebench_race.h"
#include "../include/gatebench_gate.h"
#include "../include/gatebench_nl.h"
#include "../include/gatebench_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RACE_SEED_BASE 0x5a17c3d1u
#define RACE_MIN_PKT 64u
#define RACE_MAX_PKT 1500u

struct gb_race_nl_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    uint32_t seed;
    uint32_t index;
    uint32_t max_entries;
    uint32_t interval_max;
    int timeout_ms;
    uint64_t ops;
    uint64_t errors;
};

struct gb_race_dump_ctx {
    const struct gb_config* cfg;
    atomic_bool* stop;
    uint32_t index;
    int timeout_ms;
    uint64_t ops;
    uint64_t errors;
};

struct gb_race_traffic_ctx {
    atomic_bool* stop;
    uint32_t seed;
    uint64_t ops;
    uint64_t errors;
};

static uint32_t rng_next(uint32_t* state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static uint32_t rng_range(uint32_t* state, uint32_t max) {
    if (max == 0)
        return 0;
    return (uint32_t)(((uint64_t)rng_next(state) * (uint64_t)max) >> 32);
}

static int32_t race_random_maxoctets(uint32_t* seed) {
    if (rng_range(seed, 4u) == 0u)
        return -1;
    return (int32_t)(RACE_MIN_PKT + rng_range(seed, 65535u - RACE_MIN_PKT));
}

static void race_shape_init(struct gate_shape* shape, const struct gb_config* cfg) {
    memset(shape, 0, sizeof(*shape));
    shape->clockid = cfg->clockid;
    shape->base_time = cfg->base_time;
    shape->cycle_time = cfg->cycle_time;
    shape->cycle_time_ext = cfg->cycle_time_ext;
    shape->interval_ns = cfg->interval_ns;
    shape->entries = cfg->entries;
}

static uint32_t race_fill_entries(struct gate_entry* entries,
                                  uint32_t max_entries,
                                  uint32_t interval_max,
                                  uint32_t* seed) {
    uint32_t count = 1u + rng_range(seed, max_entries);

    if (interval_max == 0)
        interval_max = 1;

    for (uint32_t i = 0; i < count; i++) {
        entries[i].index = i;
        entries[i].interval = 1u + rng_range(seed, interval_max);
        entries[i].gate_state = rng_range(seed, 2u) != 0u;
        entries[i].ipv = -1;
        entries[i].maxoctets = race_random_maxoctets(seed);
    }

    return count;
}

static void* race_replace_thread(void* arg) {
    struct gb_race_nl_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* req = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_entry* entries = NULL;
    struct gate_shape shape;
    size_t cap;
    int ret;

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        ctx->errors++;
        return NULL;
    }

    entries = calloc(ctx->max_entries, sizeof(*entries));
    if (!entries) {
        ctx->errors++;
        gb_nl_close(sock);
        return NULL;
    }

    cap = gate_msg_capacity(ctx->max_entries, 0);
    req = gb_nl_msg_alloc(cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!req || !resp) {
        ctx->errors++;
        goto out;
    }

    race_shape_init(&shape, ctx->cfg);

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        uint32_t count = race_fill_entries(entries, ctx->max_entries, ctx->interval_max, &ctx->seed);

        ret = build_gate_newaction(req, ctx->index, &shape, entries, count, NLM_F_CREATE | NLM_F_REPLACE, 0, -1);
        if (ret < 0) {
            ctx->errors++;
            continue;
        }

        ret = gb_nl_send_recv(sock, req, resp, ctx->timeout_ms);
        if (ret < 0 && ret != -EEXIST && ret != -ENOENT)
            ctx->errors++;

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

out:
    free(entries);
    if (req)
        gb_nl_msg_free(req);
    if (resp)
        gb_nl_msg_free(resp);
    gb_nl_close(sock);
    return NULL;
}

static void* race_dump_thread(void* arg) {
    struct gb_race_dump_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* req = NULL;
    struct gb_dump_stats stats;
    int ret;

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        ctx->errors++;
        return NULL;
    }

    req = gb_nl_msg_alloc(1024u);
    if (!req) {
        ctx->errors++;
        gb_nl_close(sock);
        return NULL;
    }

    ret = build_gate_getaction_ex(req, ctx->index, NLM_F_DUMP);
    if (ret < 0) {
        ctx->errors++;
        gb_nl_msg_free(req);
        gb_nl_close(sock);
        return NULL;
    }

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        ret = gb_nl_dump_action(sock, req, &stats, ctx->timeout_ms);
        if (ret < 0 || stats.saw_error)
            ctx->errors++;

        ctx->ops++;
        if ((ctx->ops & 0xffu) == 0u)
            usleep(100);
    }

    gb_nl_msg_free(req);
    gb_nl_close(sock);
    return NULL;
}

static void* race_delete_thread(void* arg) {
    struct gb_race_nl_ctx* ctx = arg;
    struct gb_nl_sock* sock = NULL;
    struct gb_nl_msg* del_msg = NULL;
    struct gb_nl_msg* create_msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_entry* entries = NULL;
    struct gate_shape shape;
    size_t create_cap;
    int ret;

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        ctx->errors++;
        return NULL;
    }

    entries = calloc(ctx->max_entries, sizeof(*entries));
    if (!entries) {
        ctx->errors++;
        gb_nl_close(sock);
        return NULL;
    }

    del_msg = gb_nl_msg_alloc(1024u);
    create_cap = gate_msg_capacity(ctx->max_entries, 0);
    create_msg = gb_nl_msg_alloc(create_cap);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!del_msg || !create_msg || !resp) {
        ctx->errors++;
        goto out;
    }

    ret = build_gate_delaction(del_msg, ctx->index);
    if (ret < 0) {
        ctx->errors++;
        goto out;
    }

    race_shape_init(&shape, ctx->cfg);

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        ret = gb_nl_send_recv(sock, del_msg, resp, ctx->timeout_ms);
        if (ret < 0 && ret != -ENOENT)
            ctx->errors++;

        {
            uint32_t count = race_fill_entries(entries, ctx->max_entries, ctx->interval_max, &ctx->seed);
            ret =
                build_gate_newaction(create_msg, ctx->index, &shape, entries, count, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
        }
        if (ret < 0) {
            ctx->errors++;
            continue;
        }

        ret = gb_nl_send_recv(sock, create_msg, resp, ctx->timeout_ms);
        if (ret < 0 && ret != -EEXIST)
            ctx->errors++;

        ctx->ops++;
        usleep(100);
    }

out:
    free(entries);
    if (del_msg)
        gb_nl_msg_free(del_msg);
    if (create_msg)
        gb_nl_msg_free(create_msg);
    if (resp)
        gb_nl_msg_free(resp);
    gb_nl_close(sock);
    return NULL;
}

static void* race_traffic_thread(void* arg) {
    struct gb_race_traffic_ctx* ctx = arg;
    int fd;
    struct sockaddr_in addr;
    char payload[RACE_MAX_PKT];
    struct timeval tv;
    ssize_t ret;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        ctx->errors++;
        return NULL;
    }

    memset(&tv, 0, sizeof(tv));
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    memset(payload, 0x5a, sizeof(payload));

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        uint32_t span = RACE_MAX_PKT - RACE_MIN_PKT + 1u;
        uint32_t len = RACE_MIN_PKT + rng_range(&ctx->seed, span);

        ret = sendto(fd, payload, len, 0, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0)
            ctx->errors++;
        else
            ctx->ops++;

        if ((ctx->ops & 0xfffu) == 0u)
            usleep(100);
    }

    close(fd);
    return NULL;
}

int gb_race_run(const struct gb_config* cfg) {
    atomic_bool stop = ATOMIC_VAR_INIT(false);
    struct gb_race_nl_ctx replace_ctx;
    struct gb_race_dump_ctx dump_ctx;
    struct gb_race_nl_ctx delete_ctx;
    struct gb_race_traffic_ctx traffic_ctx;
    pthread_t replace_thread;
    pthread_t dump_thread;
    pthread_t delete_thread;
    pthread_t traffic_thread;
    uint32_t base_interval;
    uint32_t interval_max;
    uint32_t max_entries;
    uint64_t sleep_ns;
    int ret;
    int created = 0;

    if (!cfg)
        return -EINVAL;

    if (cfg->race_seconds == 0)
        return -EINVAL;

    max_entries = cfg->entries == 0 ? 1u : cfg->entries;
    if (cfg->interval_ns == 0 || cfg->interval_ns > UINT32_MAX)
        base_interval = 1000000u;
    else
        base_interval = (uint32_t)cfg->interval_ns;

    if (base_interval < (UINT32_MAX / 2u))
        interval_max = base_interval * 2u;
    else
        interval_max = UINT32_MAX;

    replace_ctx = (struct gb_race_nl_ctx){
        .cfg = cfg,
        .stop = &stop,
        .seed = RACE_SEED_BASE ^ 0x11111111u,
        .index = cfg->index,
        .max_entries = max_entries,
        .interval_max = interval_max,
        .timeout_ms = cfg->timeout_ms,
        .ops = 0,
        .errors = 0,
    };

    dump_ctx = (struct gb_race_dump_ctx){
        .cfg = cfg,
        .stop = &stop,
        .index = cfg->index,
        .timeout_ms = cfg->timeout_ms,
        .ops = 0,
        .errors = 0,
    };

    delete_ctx = (struct gb_race_nl_ctx){
        .cfg = cfg,
        .stop = &stop,
        .seed = RACE_SEED_BASE ^ 0x33333333u,
        .index = cfg->index,
        .max_entries = max_entries,
        .interval_max = interval_max,
        .timeout_ms = cfg->timeout_ms,
        .ops = 0,
        .errors = 0,
    };

    traffic_ctx = (struct gb_race_traffic_ctx){
        .stop = &stop,
        .seed = RACE_SEED_BASE ^ 0x77777777u,
        .ops = 0,
        .errors = 0,
    };

    ret = pthread_create(&replace_thread, NULL, race_replace_thread, &replace_ctx);
    if (ret != 0)
        return -ret;
    created++;

    ret = pthread_create(&dump_thread, NULL, race_dump_thread, &dump_ctx);
    if (ret != 0)
        goto out_stop;
    created++;

    ret = pthread_create(&traffic_thread, NULL, race_traffic_thread, &traffic_ctx);
    if (ret != 0)
        goto out_stop;
    created++;

    ret = pthread_create(&delete_thread, NULL, race_delete_thread, &delete_ctx);
    if (ret != 0)
        goto out_stop;
    created++;

    sleep_ns = (uint64_t)cfg->race_seconds * 1000000000ull;
    (void)gb_util_sleep_ns(sleep_ns);

out_stop:
    atomic_store_explicit(&stop, true, memory_order_relaxed);

    if (created > 0)
        pthread_join(replace_thread, NULL);
    if (created > 1)
        pthread_join(dump_thread, NULL);
    if (created > 2)
        pthread_join(traffic_thread, NULL);
    if (created > 3)
        pthread_join(delete_thread, NULL);

    if (!cfg->json) {
        if (ret == 0) {
            printf("Race mode completed (%u seconds)\n", cfg->race_seconds);
        }
        else {
            printf("Race mode stopped early: %s (%d)\n", strerror(ret), ret);
        }
        printf("  Replace ops: %llu, errors: %llu\n", (unsigned long long)replace_ctx.ops,
               (unsigned long long)replace_ctx.errors);
        printf("  Dump ops:    %llu, errors: %llu\n", (unsigned long long)dump_ctx.ops,
               (unsigned long long)dump_ctx.errors);
        printf("  Traffic ops: %llu, errors: %llu\n", (unsigned long long)traffic_ctx.ops,
               (unsigned long long)traffic_ctx.errors);
        printf("  Delete ops:  %llu, errors: %llu\n", (unsigned long long)delete_ctx.ops,
               (unsigned long long)delete_ctx.errors);
    }

    if (ret != 0)
        return -ret;
    return 0;
}
