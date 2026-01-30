#include "../include/gatebench_selftest.h"
#include "selftests/selftest_common.h"
#include "selftests/selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct gb_selftest_case tests[] = {
    {"create missing parms", gb_selftest_create_missing_parms, -EINVAL},
    {"create missing entry list", gb_selftest_create_missing_entries, -EINVAL},
    {"create empty entry list", gb_selftest_create_empty_entries, -EINVAL},
    {"create zero interval", gb_selftest_create_zero_interval, -EINVAL},
    {"create bad clockid", gb_selftest_create_bad_clockid, -EINVAL},
    {"replace without existing", gb_selftest_replace_without_existing, 0},
    {"duplicate create", gb_selftest_duplicate_create, -EEXIST},
    {"dump correctness", gb_selftest_dump_correctness, 0},
    {"replace persistence", gb_selftest_replace_persistence, 0},
    {"clockid variants", gb_selftest_clockid_variants, 0},
    {"cycle time derivation", gb_selftest_cycle_time_derivation, 0},
    {"cycle time extension parsing", gb_selftest_cycle_time_ext_parsing, 0},
    {"replace preserve schedule", gb_selftest_replace_preserve_schedule, 0},
    {"base time update", gb_selftest_base_time_update, 0},
    {"multiple entries", gb_selftest_multiple_entries, 0},
    {"malformed nesting", gb_selftest_malformed_nesting, -EINVAL},
    {"bad attribute size", gb_selftest_bad_attribute_size, -EINVAL},
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

int gb_selftest_run(const struct gb_config* cfg) {
    struct gb_nl_sock* sock = NULL;
    uint32_t base_index;
    int ret;
    size_t i;
    int passed = 0;

    ret = gb_nl_open(&sock);
    if (ret < 0) {
        fprintf(stderr, "Failed to open netlink socket: %s\n", strerror(-ret));
        return ret;
    }

    base_index = cfg->index;

    printf("Running %zu selftests...\n", NUM_TESTS);

    for (i = 0; i < NUM_TESTS; i++) {
        uint32_t test_index = base_index + (uint32_t)(i * 1024);

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

    printf("\nSelftests: %d/%zu passed\n", passed, NUM_TESTS);

    gb_nl_close(sock);

    if (passed == (int)NUM_TESTS) {
        return 0;
    }

    return -EINVAL;
}
