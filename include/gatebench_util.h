/* include/gatebench_util.h
 * Public API for utility functions.
 */
#ifndef GATEBENCH_UTIL_H
#define GATEBENCH_UTIL_H

#include <stdint.h>

/* CPU pinning */
int gb_util_pin_cpu(int cpu);

/* High-resolution timing (returns 0 on success, -errno on failure). */
int gb_util_ns_now(uint64_t* out_ns, int clockid);

/* Priority setting */
int gb_util_set_priority(int priority);

/* Get current CPU (returns 0 on success, -errno on failure). */
int gb_util_get_cpu(int* out_cpu);

/* Nanosecond sleep (returns 0 on success, -errno on failure). */
int gb_util_sleep_ns(uint64_t ns);

/* String parsing utilities */
int gb_util_parse_uint64(const char* str, uint64_t* val);
int gb_util_parse_uint32(const char* str, uint32_t* val);

/* Clock ID to string */
const char* gb_util_clockid_name(int clockid);

#endif /* GATEBENCH_UTIL_H */
