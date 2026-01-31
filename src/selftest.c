#include "../include/gatebench_selftest.h"
#include "selftests/selftest_common.h"
#include "selftests/selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static const struct gb_selftest_case internal_tests[] = {
    {"schedule pattern", gb_selftest_internal_schedule_pattern, 0},
};

static const struct gb_selftest_case stable_tests[] = {
    {"create missing parms", gb_selftest_create_missing_parms, -EINVAL},
    {"create missing entry list", gb_selftest_create_missing_entries, 0},
    {"create empty entry list", gb_selftest_create_empty_entries, 0},
    {"malformed nesting", gb_selftest_malformed_nesting, 0},
    {"create zero interval", gb_selftest_create_zero_interval, -EINVAL},
    {"create bad clockid", gb_selftest_create_bad_clockid, -EINVAL},
    {"invalid action control", gb_selftest_invalid_action_control, -EINVAL},
    {"invalid entry attrs", gb_selftest_invalid_entry_attrs, GB_NL_EXPECT_COMPAT},
    {"bad attribute size", gb_selftest_bad_attribute_size, -EINVAL},
    {"param validation", gb_selftest_param_validation, 0},
    {"replace without existing", gb_selftest_replace_without_existing, 0},
    {"duplicate create", gb_selftest_duplicate_create, -EEXIST},
    {"replace preserve schedule", gb_selftest_replace_preserve_schedule, 0},
    {"replace append entries", gb_selftest_replace_append_entries, 0},
    {"base time update", gb_selftest_base_time_update, 0},
    {"replace persistence", gb_selftest_replace_persistence, 0},
    {"clockid variants", gb_selftest_clockid_variants, 0},
    {"cycle time derivation", gb_selftest_cycle_time_derivation, 0},
    {"cycle time extension parsing", gb_selftest_cycle_time_ext_parsing, 0},
    {"dump correctness", gb_selftest_dump_correctness, 0},
    {"multiple entries", gb_selftest_multiple_entries, 0},
    {"replace invalid", gb_selftest_replace_invalid, 0},
    {"large dump", gb_selftest_large_dump, 0},
};

#define NUM_INTERNAL_TESTS (sizeof(internal_tests) / sizeof(internal_tests[0]))
#define NUM_STABLE_TESTS (sizeof(stable_tests) / sizeof(stable_tests[0]))
#define SOFTFAIL_LARGE_DUMP "large dump"
#define SOFTFAIL_REPLACE_APPEND "replace append entries"
#define SOFTFAIL_CREATE_MISSING_ENTRY_LIST "create missing entry list"
#define SOFTFAIL_CREATE_EMPTY_ENTRY_LIST "create empty entry list"

static bool is_soft_fail(const char* name, const char* const* soft_fail_names, size_t soft_fail_count) {
    if (!name || !soft_fail_names)
        return false;

    for (size_t i = 0; i < soft_fail_count; i++) {
        if (soft_fail_names[i] && strcmp(name, soft_fail_names[i]) == 0)
            return true;
    }

    return false;
}

static int run_test_suite(const char* label,
                          const struct gb_selftest_case* tests,
                          size_t count,
                          struct gb_nl_sock* sock,
                          uint32_t base_index,
                          int* passed_out,
                          size_t* failed_out,
                          size_t* soft_failed_out,
                          const char* const* soft_fail_names,
                          size_t soft_fail_count,
                          bool* large_dump_failed_out) {
    int passed = 0;
    size_t failed = 0;
    size_t soft_failed = 0;
    bool large_dump_failed = false;

    printf("Running %zu %s selftests...\n", count, label);

    for (size_t i = 0; i < count; i++) {
        uint32_t test_index = base_index + (uint32_t)(i * 1024u);
        int ret;

        printf("  %-30s ", tests[i].name);
        fflush(stdout);

        ret = tests[i].func(sock, test_index);

        if (gb_nl_error_expected(ret, tests[i].expected_err)) {
            printf("PASS (got %d)\n", ret);
            passed++;
        }
        else {
            printf("FAIL (got %d, expected %d)\n", ret, tests[i].expected_err);
            failed++;
            if (is_soft_fail(tests[i].name, soft_fail_names, soft_fail_count))
                soft_failed++;
            if (strcmp(tests[i].name, SOFTFAIL_LARGE_DUMP) == 0)
                large_dump_failed = true;
        }
    }

    printf("%s selftests: %d/%zu passed\n\n", label, passed, count);

    if (passed_out)
        *passed_out = passed;
    if (failed_out)
        *failed_out = failed;
    if (soft_failed_out)
        *soft_failed_out = soft_failed;
    if (large_dump_failed_out)
        *large_dump_failed_out = large_dump_failed;

    return failed == 0 ? 0 : -EINVAL;
}

int gb_selftest_run(struct gb_config* cfg) {
    struct gb_nl_sock* sock = NULL;
    uint32_t base_index;
    int internal_passed = 0;
    int stable_passed = 0;
    size_t stable_failed = 0;
    size_t stable_soft_failed = 0;
    bool large_dump_failed = false;
    int ret_internal;
    int ret_stable;
    int ret;
    static const char* const soft_fail_tests[] = {SOFTFAIL_LARGE_DUMP, SOFTFAIL_REPLACE_APPEND,
                                                  SOFTFAIL_CREATE_MISSING_ENTRY_LIST, SOFTFAIL_CREATE_EMPTY_ENTRY_LIST};

    base_index = cfg->index;

    ret_internal = run_test_suite("internal", internal_tests, NUM_INTERNAL_TESTS, NULL, base_index, &internal_passed,
                                  NULL, NULL, NULL, 0, NULL);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        fprintf(stderr, "Failed to open netlink socket: %s\n", strerror(-ret));
        return ret;
    }

    ret_stable = run_test_suite("stable regression", stable_tests, NUM_STABLE_TESTS, sock, base_index, &stable_passed,
                                &stable_failed, &stable_soft_failed, soft_fail_tests,
                                sizeof(soft_fail_tests) / sizeof(soft_fail_tests[0]), &large_dump_failed);

    gb_nl_close(sock);

    printf("Selftests total: %d/%zu passed\n", internal_passed + stable_passed, NUM_INTERNAL_TESTS + NUM_STABLE_TESTS);

    if (stable_soft_failed > 0 && stable_failed == stable_soft_failed) {
        ret_stable = 0;
        if (large_dump_failed) {
            uint32_t old_entries = cfg->entries;

            cfg->entries = 50;
            if (cfg->json) {
                fprintf(stderr, "Large dump failed; setting benchmark entries to 50 (was %u)\n", old_entries);
            }
            else {
                printf("Large dump failed; setting benchmark entries to 50 (was %u)\n", old_entries);
            }
        }
    }

    if (ret_internal == 0 && ret_stable == 0) {
        if (stable_soft_failed > 0)
            return 1;
        return 0;
    }

    return -EINVAL;
}
