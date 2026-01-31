#include "selftest_tests.h"
#include "../bench_internal.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>

int gb_selftest_internal_schedule_pattern(struct gb_nl_sock* sock, uint32_t base_index) {
    const uint32_t entries = 20u;
    const uint64_t interval_ns = 100000u;
    struct gate_entry schedule[20];
    struct gate_entry small[8];
    int ret;

    (void)sock;
    (void)base_index;

    memset(schedule, 0, sizeof(schedule));
    ret = gb_fill_entries(schedule, entries, interval_ns);
    if (ret < 0)
        return ret;

    for (uint32_t i = 0; i < entries; i++) {
        bool guard_slot = ((i + 1u) % 10u) == 0u;

        if (schedule[i].index != i)
            return -EINVAL;

        if (schedule[i].interval != (uint32_t)interval_ns)
            return -EINVAL;

        if (guard_slot) {
            if (schedule[i].gate_state)
                return -EINVAL;
            if (schedule[i].ipv != -1 || schedule[i].maxoctets != -1)
                return -EINVAL;
            continue;
        }

        if (!schedule[i].gate_state)
            return -EINVAL;

        if ((i % 2u) == 0u) {
            if (schedule[i].ipv != 7 || schedule[i].maxoctets != 8192)
                return -EINVAL;
        }
        else {
            if (schedule[i].ipv != 0 || schedule[i].maxoctets != 32768)
                return -EINVAL;
        }
    }

    memset(small, 0, sizeof(small));
    ret = gb_fill_entries(small, 8u, interval_ns);
    if (ret < 0)
        return ret;

    for (uint32_t i = 0; i < 8u; i++) {
        if (small[i].index != i)
            return -EINVAL;
        if (!small[i].gate_state)
            return -EINVAL;
        if (small[i].interval != (uint32_t)interval_ns)
            return -EINVAL;
        if ((i % 2u) == 0u) {
            if (small[i].ipv != 7 || small[i].maxoctets != 8192)
                return -EINVAL;
        }
        else {
            if (small[i].ipv != 0 || small[i].maxoctets != 32768)
                return -EINVAL;
        }
    }

    ret = gb_fill_entries(schedule, entries, 0);
    if (ret != -ERANGE)
        return -EINVAL;

    return 0;
}
