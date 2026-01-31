#include "../include/gatebench_selftest.h"
#include "selftests/selftest_common.h"
#include "selftests/selftest_tests.h"
#include <errno.h>
#include <stdio.h>
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

static int run_test_suite(const char* label,
                          const struct gb_selftest_case* tests,
                          size_t count,
                          struct gb_nl_sock* sock,
                          uint32_t base_index,
                          int* passed_out) {
    int passed = 0;

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
        }
    }

    printf("%s selftests: %d/%zu passed\n\n", label, passed, count);

    if (passed_out)
        *passed_out = passed;

    return passed == (int)count ? 0 : -EINVAL;
}

int gb_selftest_run(const struct gb_config* cfg) {
    struct gb_nl_sock* sock = NULL;
    uint32_t base_index;
    int internal_passed = 0;
    int stable_passed = 0;
    int ret_internal;
    int ret_stable;
    int ret;

    base_index = cfg->index;

    ret_internal = run_test_suite("internal", internal_tests, NUM_INTERNAL_TESTS, NULL, base_index, &internal_passed);

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        fprintf(stderr, "Failed to open netlink socket: %s\n", strerror(-ret));
        return ret;
    }

    ret_stable = run_test_suite("stable regression", stable_tests, NUM_STABLE_TESTS, sock, base_index, &stable_passed);

    gb_nl_close(sock);

    printf("Selftests total: %d/%zu passed\n", internal_passed + stable_passed, NUM_INTERNAL_TESTS + NUM_STABLE_TESTS);

    if (ret_internal == 0 && ret_stable == 0)
        return 0;

    return -EINVAL;
}
