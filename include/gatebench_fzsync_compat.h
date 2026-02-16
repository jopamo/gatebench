/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Compatibility layer for using LTP's tst_fuzzy_sync.h in gatebench.
 */
#ifndef GATEBENCH_FZSYNC_COMPAT_H
#define GATEBENCH_FZSYNC_COMPAT_H

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef int tst_atomic_t;

enum tst_res_type {
    TINFO = 1,
    TWARN = 2,
    TBROK = 3,
};

static inline bool* gb_fzsync_info_enabled_ptr(void) {
    static bool enabled = false;
    return &enabled;
}

static inline void gb_fzsync_set_info(bool enabled) {
    *gb_fzsync_info_enabled_ptr() = enabled;
}

static inline int tst_atomic_inc(int* v) {
    return __atomic_add_fetch(v, 1, __ATOMIC_RELAXED);
}

static inline int tst_atomic_load(const int* v) {
    return __atomic_load_n(v, __ATOMIC_RELAXED);
}

static inline void tst_atomic_store(int value, int* v) {
    __atomic_store_n(v, value, __ATOMIC_RELAXED);
}

static inline int tst_ncpus_available(void) {
    cpu_set_t set;
    int count = 0;
    long nproc;

    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (CPU_ISSET((unsigned int)cpu, &set))
                count++;
        }
        if (count > 0)
            return count;
    }

    nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0)
        return 1;
    if (nproc > INT_MAX)
        return INT_MAX;
    return (int)nproc;
}

static inline atomic_uint_fast64_t* gb_fzsync_rng_state_ptr(void) {
    static atomic_uint_fast64_t state = ATOMIC_VAR_INIT(UINT64_C(0x9e3779b97f4a7c15));
    return &state;
}

static inline void gb_fzsync_seed(uint64_t seed) {
    if (seed == 0)
        seed = UINT64_C(0x9e3779b97f4a7c15);
    atomic_store_explicit(gb_fzsync_rng_state_ptr(), seed, memory_order_relaxed);
}

static inline double gb_fzsync_drand48(void) {
    uint64_t cur;
    uint64_t next;
    const double inv = 1.0 / 9007199254740992.0; /* 2^53 */

    cur = atomic_load_explicit(gb_fzsync_rng_state_ptr(), memory_order_relaxed);
    do {
        next = cur * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    } while (!atomic_compare_exchange_weak_explicit(gb_fzsync_rng_state_ptr(), &cur, next, memory_order_relaxed,
                                                    memory_order_relaxed));

    return (double)(next >> 11) * inv;
}

#define drand48() gb_fzsync_drand48()

static inline float tst_remaining_runtime(void) {
    /*
     * gatebench controls race mode lifetime explicitly, so the fzsync run_a()
     * path is not used for stop decisions.
     */
    return 1.0f;
}

static inline float tst_timespec_diff_ns(struct timespec t1, struct timespec t2) {
    int64_t sec = (int64_t)t1.tv_sec - (int64_t)t2.tv_sec;
    int64_t nsec = (int64_t)t1.tv_nsec - (int64_t)t2.tv_nsec;
    return (float)((double)sec * 1000000000.0 + (double)nsec);
}

static inline void gb_fzsync_logv(const char* prefix, const char* fmt, va_list ap) {
    char line[512];

    if (!fmt)
        return;

    if (vsnprintf(line, sizeof(line), fmt, ap) < 0)
        return;

    if (prefix && prefix[0] != '\0')
        fprintf(stderr, "fzsync:%s ", prefix);
    else
        fprintf(stderr, "fzsync: ");
    fprintf(stderr, "%s", line);
    fputc('\n', stderr);
}

static inline void tst_res(int type, const char* fmt, ...) {
    va_list ap;
    const char* prefix;

    if (type == TINFO && !*gb_fzsync_info_enabled_ptr())
        return;

    prefix = (type == TINFO) ? "info" : ((type == TWARN) ? "warn" : "log");

    va_start(ap, fmt);
    gb_fzsync_logv(prefix, fmt, ap);
    va_end(ap);
}

static inline void tst_brk(int type, const char* fmt, ...) {
    va_list ap;
    (void)type;

    va_start(ap, fmt);
    gb_fzsync_logv("fatal", fmt, ap);
    va_end(ap);
    abort();
}

static inline void gb_fzsync_safe_pthread_create(pthread_t* thread,
                                                 const pthread_attr_t* attr,
                                                 void* (*run)(void*),
                                                 void* arg) {
    int ret = pthread_create(thread, attr, run, arg);
    if (ret != 0) {
        fprintf(stderr, "fzsync:fatal pthread_create failed: %s\n", strerror(ret));
        abort();
    }
}

static inline void gb_fzsync_safe_pthread_join(pthread_t thread, void** retp) {
    int ret = pthread_join(thread, retp);
    if (ret != 0) {
        fprintf(stderr, "fzsync:fatal pthread_join failed: %s\n", strerror(ret));
        abort();
    }
}

#define SAFE_PTHREAD_CREATE(thread, attr, run, arg) gb_fzsync_safe_pthread_create((thread), (attr), (run), (arg))

#define SAFE_PTHREAD_JOIN(thread, retp) gb_fzsync_safe_pthread_join((thread), (retp))

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif /* GATEBENCH_FZSYNC_COMPAT_H */
