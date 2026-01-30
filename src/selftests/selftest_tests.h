#ifndef GATEBENCH_SELFTEST_TESTS_H
#define GATEBENCH_SELFTEST_TESTS_H

#include "selftest_common.h"

int gb_selftest_create_missing_parms(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_create_missing_entries(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_create_empty_entries(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_create_zero_interval(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_create_bad_clockid(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_replace_without_existing(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_duplicate_create(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_dump_correctness(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_replace_persistence(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_clockid_variants(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_cycle_time_derivation(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_cycle_time_ext_parsing(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_replace_preserve_schedule(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_base_time_update(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_multiple_entries(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_malformed_nesting(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_bad_attribute_size(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_param_validation(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_replace_invalid(struct gb_nl_sock* sock, uint32_t base_index);
int gb_selftest_large_dump(struct gb_nl_sock* sock, uint32_t base_index);

#endif /* GATEBENCH_SELFTEST_TESTS_H */
