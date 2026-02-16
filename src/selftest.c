/* src/selftest.c
 * Self-test routines to validate environment before benchmarking.
 */
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
    {"replace RCU snapshot", gb_selftest_replace_rcu_snapshot, 0},
    {"gate timer start logic", gb_selftest_gate_timer_start_logic, 0},
    {"base time update", gb_selftest_base_time_update, 0},
    {"replace persistence", gb_selftest_replace_persistence, 0},
    {"replace preserve attrs", gb_selftest_replace_preserve_attrs, 0},
    {"attr presence matrix", gb_selftest_attr_matrix, 0},
    {"create attr matrix", gb_selftest_attr_matrix_create, 0},
    {"unknown attrs", gb_selftest_unknown_attrs, 0},
    {"extreme time values", gb_selftest_extreme_time_values, 0},
    {"cycle sum overflow", gb_selftest_cycle_sum_overflow, 0},
    {"entry index attrs", gb_selftest_entry_index_attrs, 0},
    {"mixed invalid entries", gb_selftest_mixed_invalid_entries, 0},
    {"clockid variants", gb_selftest_clockid_variants, 0},
    {"cycle time derivation", gb_selftest_cycle_time_derivation, 0},
    {"cycle time extension parsing", gb_selftest_cycle_time_ext_parsing, 0},
    {"cycle time supplied", gb_selftest_cycle_time_supplied, 0},
    {"dump correctness", gb_selftest_dump_correctness, 0},
    {"priority and flags", gb_selftest_priority_flags, 0},
    {"entry defaults", gb_selftest_entry_defaults, 0},
    {"multiple entries", gb_selftest_multiple_entries, 0},
    {"entry corner cases", gb_selftest_entry_corner_cases, 0},
    {"replace invalid", gb_selftest_replace_invalid, 0},
};

static const struct gb_selftest_case historical_tests[] = {
    {"create missing entry list", gb_selftest_create_missing_entries, 0},
    {"create empty entry list", gb_selftest_create_empty_entries, 0},
    {"replace append entries", gb_selftest_replace_append_entries, 0},
};

static const struct gb_selftest_case unpatched_tests[] = {
    {"large dump", gb_selftest_large_dump, 0},
};

#define NUM_INTERNAL_TESTS (sizeof(internal_tests) / sizeof(internal_tests[0]))
#define NUM_STABLE_TESTS (sizeof(stable_tests) / sizeof(stable_tests[0]))
#define NUM_HISTORICAL_TESTS (sizeof(historical_tests) / sizeof(historical_tests[0]))
#define NUM_UNPATCHED_TESTS (sizeof(unpatched_tests) / sizeof(unpatched_tests[0]))
#define UNPATCHED_LARGE_DUMP "large dump"
#define HISTORICAL_REPLACE_APPEND "replace append entries"
#define HISTORICAL_CREATE_MISSING_ENTRY_LIST "create missing entry list"
#define HISTORICAL_CREATE_EMPTY_ENTRY_LIST "create empty entry list"

static bool is_soft_fail(const char* name, const char* const* soft_fail_names, size_t soft_fail_count) {
    if (!name || !soft_fail_names)
        return false;

    for (size_t i = 0; i < soft_fail_count; i++) {
        if (soft_fail_names[i] && strcmp(name, soft_fail_names[i]) == 0)
            return true;
    }

    return false;
}

static void print_suite_summary(const char* label, int passed, size_t count, size_t failed, size_t soft_failed) {
    size_t hard_failed = 0;

    if (!label)
        label = "unknown";

    if (failed > soft_failed)
        hard_failed = failed - soft_failed;

    printf("  %-20s %d/%zu", label, passed, count);
    if (hard_failed > 0 || soft_failed > 0) {
        printf(" (");
        if (hard_failed > 0) {
            printf("%zu fail%s", hard_failed, hard_failed == 1 ? "" : "s");
            if (soft_failed > 0)
                printf(", ");
        }
        if (soft_failed > 0)
            printf("%zu soft-fail%s", soft_failed, soft_failed == 1 ? "" : "s");
        printf(")");
    }
    printf("\n");
}

static void print_test_result_line(const char* name, const char* status, int ret, int expected, bool show_expected) {
    const int name_width = 32;

    if (!name || !status)
        return;

    printf("    %-*s %s (got %d", name_width, name, status, ret);
    if (show_expected)
        printf(", expected %d", expected);
    printf(")\n");
}

static int run_test_suite(const char* label,
                          const char* summary_label,
                          const struct gb_selftest_case* tests,
                          size_t count,
                          struct gb_nl_sock* sock,
                          uint32_t base_index,
                          int* passed_out,
                          size_t* failed_out,
                          size_t* soft_failed_out,
                          const char* const* soft_fail_names,
                          size_t soft_fail_count,
                          bool* large_dump_failed_out,
                          bool verbose,
                          bool quiet) {
    int passed = 0;
    size_t failed = 0;
    size_t soft_failed = 0;
    bool large_dump_failed = false;
    const char* suite_label = summary_label ? summary_label : label;

    if (!quiet) {
        if (verbose)
            printf("== %s selftests (%zu) ==\n", label, count);
        else
            printf("  %s (%zu):\n", suite_label ? suite_label : "unknown", count);
    }

    for (size_t i = 0; i < count; i++) {
        uint32_t test_index = base_index + (uint32_t)(i * 1024u);
        int ret;
        bool passed_ok;
        bool is_soft = false;
        const char* status = "FAIL";

        if (!quiet && verbose)
            printf("  - %s\n", tests[i].name);

        ret = tests[i].func(sock, test_index);

        passed_ok = gb_nl_error_expected(ret, tests[i].expected_err);
        if (passed_ok) {
            if (!quiet && verbose)
                printf("    [PASS] got %d\n", ret);
            passed++;
            status = "PASS";
        }
        else {
            is_soft = is_soft_fail(tests[i].name, soft_fail_names, soft_fail_count);

            if (!quiet && verbose)
                printf("    [%s] got %d, expected %d\n", is_soft ? "SOFTFAIL" : "FAIL", ret, tests[i].expected_err);
            failed++;
            if (is_soft)
                soft_failed++;
            status = is_soft ? "SOFTFAIL" : "FAIL";
            if (strcmp(tests[i].name, UNPATCHED_LARGE_DUMP) == 0)
                large_dump_failed = true;
        }

        if (!quiet && !verbose)
            print_test_result_line(tests[i].name, status, ret, tests[i].expected_err, !passed_ok);
    }

    if (!quiet) {
        if (verbose) {
            printf("%s selftests: %d/%zu passed", label, passed, count);
            if (soft_failed > 0) {
                printf(" (%zu soft-fail%s)", soft_failed, soft_failed == 1 ? "" : "s");
            }
            printf("\n\n");
        }
        else {
            printf("\n");
        }
    }

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
    int historical_passed = 0;
    int unpatched_passed = 0;
    size_t stable_failed = 0;
    size_t stable_soft_failed = 0;
    size_t historical_failed = 0;
    size_t historical_soft_failed = 0;
    size_t unpatched_failed = 0;
    size_t unpatched_soft_failed = 0;
    bool large_dump_failed = false;
    int ret_internal;
    int ret_stable;
    int ret_historical;
    int ret_unpatched;
    int ret;
    bool verbose = false;
    static const char* const historical_fail_tests[] = {HISTORICAL_CREATE_MISSING_ENTRY_LIST,
                                                        HISTORICAL_CREATE_EMPTY_ENTRY_LIST, HISTORICAL_REPLACE_APPEND};
    static const char* const unpatched_fail_tests[] = {UNPATCHED_LARGE_DUMP};

    base_index = cfg->index;
    verbose = cfg->verbose && !cfg->json;
    gb_selftest_set_verbose(verbose);

    ret_internal = run_test_suite("internal", "internal", internal_tests, NUM_INTERNAL_TESTS, NULL, base_index,
                                  &internal_passed, NULL, NULL, NULL, 0, NULL, verbose, cfg->json);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        fprintf(stderr, "Failed to open netlink socket: %s\n", strerror(-ret));
        return ret;
    }

    ret_stable = run_test_suite("stable regression", "stable", stable_tests, NUM_STABLE_TESTS, sock, base_index,
                                &stable_passed, &stable_failed, &stable_soft_failed, NULL, 0, NULL, verbose, cfg->json);

    ret_historical =
        run_test_suite("historical behavior", "historical", historical_tests, NUM_HISTORICAL_TESTS, sock,
                       base_index + (uint32_t)(NUM_STABLE_TESTS * 1024u), &historical_passed, &historical_failed,
                       &historical_soft_failed, historical_fail_tests,
                       sizeof(historical_fail_tests) / sizeof(historical_fail_tests[0]), NULL, verbose, cfg->json);

    ret_unpatched = run_test_suite("unpatched behavior", "unpatched", unpatched_tests, NUM_UNPATCHED_TESTS, sock,
                                   base_index + (uint32_t)((NUM_STABLE_TESTS + NUM_HISTORICAL_TESTS) * 1024u),
                                   &unpatched_passed, &unpatched_failed, &unpatched_soft_failed, unpatched_fail_tests,
                                   sizeof(unpatched_fail_tests) / sizeof(unpatched_fail_tests[0]), &large_dump_failed,
                                   verbose, cfg->json);

    gb_nl_close(sock);

    if (!cfg->json) {
        if (verbose) {
            printf("Selftests summary: internal %d/%zu, stable %d/%zu, historical %d/%zu, unpatched %d/%zu\n",
                   internal_passed, NUM_INTERNAL_TESTS, stable_passed, NUM_STABLE_TESTS, historical_passed,
                   NUM_HISTORICAL_TESTS, unpatched_passed, NUM_UNPATCHED_TESTS);
        }
        else {
            size_t internal_failed = 0;

            if (internal_passed < (int)NUM_INTERNAL_TESTS)
                internal_failed = NUM_INTERNAL_TESTS - (size_t)internal_passed;

            printf("Summary:\n");
            print_suite_summary("internal", internal_passed, NUM_INTERNAL_TESTS, internal_failed, 0);
            print_suite_summary("stable", stable_passed, NUM_STABLE_TESTS, stable_failed, stable_soft_failed);
            print_suite_summary("historical", historical_passed, NUM_HISTORICAL_TESTS, historical_failed,
                                historical_soft_failed);
            print_suite_summary("unpatched", unpatched_passed, NUM_UNPATCHED_TESTS, unpatched_failed,
                                unpatched_soft_failed);
            printf("\n");
        }
    }

    if (stable_soft_failed > 0 && stable_failed == stable_soft_failed) {
        ret_stable = 0;
    }

    if (historical_soft_failed > 0 && historical_failed == historical_soft_failed) {
        ret_historical = 0;
    }

    if (unpatched_soft_failed > 0 && unpatched_failed == unpatched_soft_failed) {
        ret_unpatched = 0;
        if (large_dump_failed) {
            uint32_t old_entries = cfg->entries;

            if (old_entries > 50) {
                cfg->entries = 50;
                if (cfg->json) {
                    fprintf(stderr, "Note: large dump failed; setting benchmark entries to 50 (was %u)\n", old_entries);
                }
                else {
                    printf("Note: large dump failed; setting benchmark entries to 50 (was %u)\n", old_entries);
                }
            }
        }
    }

    if (ret_internal == 0 && ret_stable == 0 && ret_historical == 0 && ret_unpatched == 0) {
        if (stable_soft_failed > 0 || historical_soft_failed > 0 || unpatched_soft_failed > 0)
            return 1;
        return 0;
    }

    return -EINVAL;
}
