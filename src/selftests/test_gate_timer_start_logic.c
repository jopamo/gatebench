#include "selftest_tests.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define GB_TEST_KTIME_MAX INT64_MAX

struct gb_timer_state {
    bool active;
    int64_t expires;
};

static int64_t gate_start_old(int64_t start, const struct gb_timer_state* timer) {
    int64_t expires;

    if (!timer)
        return start;

    expires = timer->expires;
    if (expires == 0)
        expires = GB_TEST_KTIME_MAX;

    return start < expires ? start : expires;
}

static int64_t gate_start_fixed(int64_t start, const struct gb_timer_state* timer) {
    if (!timer)
        return start;

    if (!timer->active)
        return start;

    return start < timer->expires ? start : timer->expires;
}

int gb_selftest_gate_timer_start_logic(struct gb_nl_sock* sock, uint32_t base_index) {
    const int64_t ns_1ms = 1000000LL;
    const int64_t ns_10ms = 10000000LL;
    int64_t old_start;
    int64_t fixed_start;
    struct gb_timer_state timer;

    (void)sock;
    (void)base_index;

    /*
     * Trigger model:
     * 1) timer was armed in a previous cycle (expires=1ms)
     * 2) timer becomes inactive, but expires remains stale/non-zero
     * 3) new config wants start at 10ms
     * Old logic incorrectly reuses stale expires and arms too early.
     */
    timer.active = false;
    timer.expires = ns_1ms;

    old_start = gate_start_old(ns_10ms, &timer);
    fixed_start = gate_start_fixed(ns_10ms, &timer);

    gb_selftest_log("stale/inactive case old=%lld fixed=%lld expected=%lld\n", (long long)old_start,
                    (long long)fixed_start, (long long)ns_10ms);

    if (old_start != ns_1ms)
        return -EINVAL;
    if (fixed_start != ns_10ms)
        return -EINVAL;
    if (old_start >= fixed_start)
        return -EINVAL;

    /*
     * Active timer case: both logics should pick the earlier existing expiry.
     */
    timer.active = true;
    timer.expires = ns_1ms;

    old_start = gate_start_old(ns_10ms, &timer);
    fixed_start = gate_start_fixed(ns_10ms, &timer);
    if (old_start != ns_1ms || fixed_start != ns_1ms)
        return -EINVAL;

    /*
     * Freshly setup timer case (expires=0): old logic converted 0->KTIME_MAX;
     * fixed logic simply ignores expires when inactive. Both keep requested start.
     */
    timer.active = false;
    timer.expires = 0;
    old_start = gate_start_old(ns_10ms, &timer);
    fixed_start = gate_start_fixed(ns_10ms, &timer);
    if (old_start != ns_10ms || fixed_start != ns_10ms)
        return -EINVAL;

    return 0;
}
