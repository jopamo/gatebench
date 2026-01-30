#include "selftest_common.h"
#include <errno.h>
#include <string.h>
#include <time.h>

void gb_selftest_shape_default(struct gate_shape* shape, uint32_t entries) {
    if (!shape) {
        return;
    }

    memset(shape, 0, sizeof(*shape));
    shape->clockid = CLOCK_TAI;
    shape->base_time = 0;
    shape->cycle_time = GB_SELFTEST_DEFAULT_INTERVAL_NS;
    shape->cycle_time_ext = 0;
    shape->interval_ns = GB_SELFTEST_DEFAULT_INTERVAL_NS;
    shape->entries = entries;
}

void gb_selftest_entry_default(struct gate_entry* entry) {
    if (!entry) {
        return;
    }

    entry->gate_state = true;
    entry->interval = GB_SELFTEST_DEFAULT_INTERVAL_NS;
    entry->ipv = -1;
    entry->maxoctets = -1;
}

int gb_selftest_alloc_msgs(struct gb_nl_msg** msg, struct gb_nl_msg** resp, size_t capacity) {
    if (!msg || !resp) {
        return -EINVAL;
    }

    *msg = gb_nl_msg_alloc(capacity);
    *resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);
    if (!*msg || !*resp) {
        gb_selftest_free_msgs(*msg, *resp);
        return -ENOMEM;
    }

    return 0;
}

void gb_selftest_free_msgs(struct gb_nl_msg* msg, struct gb_nl_msg* resp) {
    if (msg) {
        gb_nl_msg_free(msg);
    }
    if (resp) {
        gb_nl_msg_free(resp);
    }
}

void gb_selftest_cleanup_gate(struct gb_nl_sock* sock, struct gb_nl_msg* msg, struct gb_nl_msg* resp, uint32_t index) {
    if (!sock || !msg || !resp) {
        return;
    }

    gb_nl_msg_reset(msg);
    if (build_gate_delaction(msg, index) >= 0) {
        gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    }
}
