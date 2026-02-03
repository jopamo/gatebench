/* include/gatebench_util.h
 * Public API for utility functions.
 */
#ifndef GATEBENCH_UTIL_H
#define GATEBENCH_UTIL_H

#include <stdint.h>

/* CPU pinning */
int gb_util_pin_cpu(int cpu);

/* High-resolution timing */
uint64_t gb_util_ns_now(int clockid);

/* Priority setting */
int gb_util_set_priority(int priority);

/* Get current CPU */
int gb_util_get_cpu(void);

/* Nanosecond sleep */
void gb_util_sleep_ns(uint64_t ns);

/* String parsing utilities */
int gb_util_parse_uint64(const char* str, uint64_t* val);
int gb_util_parse_uint32(const char* str, uint32_t* val);

/* Clock ID to string */
const char* gb_util_clockid_name(int clockid);

#endif /* GATEBENCH_UTIL_H */