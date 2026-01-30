#include "selftest_tests.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* Test dumping a large number of entries to check for truncation or buffer issues */
int gb_selftest_large_dump(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gate_shape shape;
    struct gate_entry* entries;
    struct gate_dump dump;
    const uint32_t num_entries = 100; /* Significant but should fit in typical netlink msg */
    int ret;
    int test_ret = 0;

    entries = calloc(num_entries, sizeof(*entries));
    if (!entries)
        return -ENOMEM;

    gb_selftest_shape_default(&shape, num_entries);
    for (uint32_t i = 0; i < num_entries; i++) {
        gb_selftest_entry_default(&entries[i]);
        entries[i].interval = 100000 + i;
        entries[i].gate_state = (i % 2) == 0;
    }

    ret = gb_selftest_alloc_msgs(&msg, &resp, gate_msg_capacity(num_entries, 0));
    if (ret < 0) {
        free(entries);
        return ret;
    }

    ret = build_gate_newaction(msg, base_index, &shape, entries, num_entries, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto out;
    }

    printf("DEBUG: msg->len = %zu\n", msg->len);

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        printf("DEBUG: create failed with %d\n", ret);
        test_ret = ret;
        goto out;
    }

    /* Verify dump */
    ret = gb_nl_get_action(sock, base_index, &dump, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        printf("DEBUG: get_action failed with %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    if (dump.num_entries != num_entries) {
        printf("Large dump failed: got %u entries, expected %u\n", dump.num_entries, num_entries);
        test_ret = -EINVAL;
    }
    else {
        /* Check a few entries */
        for (uint32_t i = 0; i < num_entries; i++) {
            if (dump.entries[i].interval != entries[i].interval ||
                dump.entries[i].gate_state != entries[i].gate_state) {
                printf("Large dump data mismatch at entry %u\n", i);
                test_ret = -EINVAL;
                break;
            }
        }
    }

    gb_gate_dump_free(&dump);

cleanup:
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

out:
    gb_selftest_free_msgs(msg, resp);
    free(entries);
    return test_ret;
}
