/* src/util.c
 * Utility functions for time, CPU pinning, etc.
 */
#include "../include/gatebench.h"
#include "../include/gatebench_util.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int gb_util_pin_cpu(int cpu) {
    cpu_set_t cpuset;

    if (cpu < 0)
        return 0; /* No pinning requested */

    if (cpu >= CPU_SETSIZE)
        return -EINVAL;

    CPU_ZERO(&cpuset);
    CPU_SET((unsigned)cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
        return -errno;

    return 0;
}

int gb_util_ns_now(uint64_t* out_ns, int clockid) {
    struct timespec ts;
    clockid_t clk = (clockid_t)clockid;

    if (!out_ns)
        return -EINVAL;

    switch (clk) {
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_TAI:
        case CLOCK_REALTIME:
        case CLOCK_BOOTTIME:
            break;
        default:
            /* Fall back to monotonic if unknown clock */
            clk = CLOCK_MONOTONIC;
            break;
    }

    if (clock_gettime(clk, &ts) < 0)
        return -errno;

    *out_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return 0;
}

int gb_util_set_priority(int priority) {
    struct sched_param param;
    int min_prio;
    int max_prio;

    if (priority < 0)
        return 0; /* No priority change requested */

    min_prio = sched_get_priority_min(SCHED_FIFO);
    if (min_prio < 0)
        return -errno;

    max_prio = sched_get_priority_max(SCHED_FIFO);
    if (max_prio < 0)
        return -errno;

    if (priority < min_prio || priority > max_prio)
        return -ERANGE;

    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;

    if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
        int err = errno;

        if (err == EPERM)
            return -err;

        /* Try SCHED_RR if FIFO fails for non-permission errors */
        if (sched_setscheduler(0, SCHED_RR, &param) < 0)
            return -errno;
    }

    return 0;
}

int gb_util_get_cpu(int* out_cpu) {
    int cpu;

    if (!out_cpu)
        return -EINVAL;

    cpu = sched_getcpu();
    if (cpu < 0)
        return -errno;

    *out_cpu = cpu;
    return 0;
}

int gb_util_sleep_ns(uint64_t ns) {
    struct timespec req, rem;

    if (ns == 0)
        return 0;

    req.tv_sec = (time_t)(ns / 1000000000ULL);
    req.tv_nsec = (long)(ns % 1000000000ULL);

    while (nanosleep(&req, &rem) < 0) {
        if (errno != EINTR)
            return -errno;
        req = rem;
    }

    return 0;
}

int gb_util_parse_uint64(const char* str, uint64_t* val) {
    char* endptr = NULL;
    uintmax_t tmp;

    if (!str || !val)
        return -EINVAL;

    errno = 0;
    tmp = strtoumax(str, &endptr, 10);
    if (endptr == str || *endptr != '\0')
        return -EINVAL;
    if (errno == ERANGE || tmp > UINT64_MAX)
        return -ERANGE;
    if (errno != 0)
        return -EINVAL;

    *val = (uint64_t)tmp;
    return 0;
}

int gb_util_parse_uint32(const char* str, uint32_t* val) {
    char* endptr = NULL;
    unsigned long tmp;

    if (!str || !val)
        return -EINVAL;

    errno = 0;
    tmp = strtoul(str, &endptr, 10);
    if (errno != 0 || endptr == str || *endptr != '\0' || tmp > UINT32_MAX)
        return -EINVAL;

    *val = (uint32_t)tmp;
    return 0;
}

const char* gb_util_clockid_name(int clockid) {
    switch ((clockid_t)clockid) {
        case CLOCK_REALTIME:
            return "CLOCK_REALTIME";
        case CLOCK_MONOTONIC:
            return "CLOCK_MONOTONIC";
        case CLOCK_MONOTONIC_RAW:
            return "CLOCK_MONOTONIC_RAW";
        case CLOCK_TAI:
            return "CLOCK_TAI";
        case CLOCK_BOOTTIME:
            return "CLOCK_BOOTTIME";
        default:
            return "UNKNOWN";
    }
}
