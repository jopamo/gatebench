#include "../include/gatebench.h"
#include "../include/gatebench_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <limits.h>

int gb_util_pin_cpu(int cpu) {
    cpu_set_t cpuset;

    if (cpu < 0) {
        return 0; /* No pinning requested */
    }

    CPU_ZERO(&cpuset);
    CPU_SET((unsigned)cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
        return -errno;
    }

    return 0;
}

uint64_t gb_util_ns_now(int clockid) {
    struct timespec ts;

    switch (clockid) {
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_TAI:
            if (clock_gettime(clockid, &ts) < 0) {
                return 0;
            }
            break;
        default:
            /* Fall back to monotonic if unknown clock */
            if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
                return 0;
            }
    }

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int gb_util_set_priority(int priority) {
    struct sched_param param;

    if (priority < 0) {
        return 0; /* No priority change requested */
    }

    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;

    if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
        /* Try SCHED_RR if FIFO fails */
        if (sched_setscheduler(0, SCHED_RR, &param) < 0) {
            return -errno;
        }
    }

    return 0;
}

int gb_util_get_cpu(void) {
    return sched_getcpu();
}

void gb_util_sleep_ns(uint64_t ns) {
    struct timespec req, rem;

    if (ns == 0) {
        return;
    }

    req.tv_sec = (time_t)(ns / 1000000000ULL);
    req.tv_nsec = (long)(ns % 1000000000ULL);

    while (nanosleep(&req, &rem) < 0) {
        if (errno == EINTR) {
            /* Interrupted by signal, continue with remaining time */
            req = rem;
            continue;
        }
        break;
    }
}

int gb_util_parse_uint64(const char* str, uint64_t* val) {
    char* endptr;
    unsigned long long tmp;

    if (!str || !val) {
        return -EINVAL;
    }

    tmp = strtoull(str, &endptr, 10);
    if (*endptr != '\0' || tmp > UINT64_MAX) {
        return -EINVAL;
    }

    *val = (uint64_t)tmp;
    return 0;
}

int gb_util_parse_uint32(const char* str, uint32_t* val) {
    char* endptr;
    unsigned long tmp;

    if (!str || !val) {
        return -EINVAL;
    }

    tmp = strtoul(str, &endptr, 10);
    if (*endptr != '\0' || tmp > UINT32_MAX) {
        return -EINVAL;
    }

    *val = (uint32_t)tmp;
    return 0;
}

const char* gb_util_clockid_name(int clockid) {
    switch (clockid) {
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