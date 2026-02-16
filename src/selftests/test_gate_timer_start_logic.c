#include "selftest_tests.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <linux/if_ether.h>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define GB_TIMER_TEST_INITIAL_CYCLE_NS 20000000ULL
#define GB_TIMER_TEST_REPLACE_CYCLE_NS 200000000ULL
#define GB_TIMER_TEST_TRIGGER_REPLACE_DELAY_NS 5000000000ULL
#define GB_TIMER_TEST_FINAL_REPLACE_DELAY_NS 1000000000ULL
#define GB_TIMER_TEST_PHASE_BASE_DELAY_NS 1500000000ULL
#define GB_TIMER_TEST_PHASE_INTERVAL_NS 250000000ULL
#define GB_TIMER_TEST_PHASE_CYCLE_NS (GB_TIMER_TEST_PHASE_INTERVAL_NS * 2ULL)
#define GB_TIMER_TEST_PHASE_OFFSET_NS 80000000ULL
#define GB_TIMER_TEST_PHASE_MAX_LATE_NS 80000000ULL
#define GB_TIMER_TEST_PHASE_CYCLES 6u
#define GB_TIMER_TEST_TRIGGER_ATTEMPTS 64u
#define GB_TIMER_TEST_TRIGGER_STEP_SLEEP_MS 5u
#define GB_TIMER_TEST_PRE_REPLACE_WAIT_MS 120u
#define GB_TIMER_TEST_PRE_BASE_GUARD_NS 100000000ULL
#define GB_TIMER_TEST_PRE_BASE_POLL_MS 25u
#define GB_TIMER_TEST_POST_BASE_CONFIRM_NS 150000000ULL
#define GB_TIMER_TEST_POST_BASE_POLL_MS 10u
#define GB_TIMER_TEST_PHASE_POLL_MS 10u
#define GB_TIMER_TEST_PROBE_TIMEOUT_US 300000
#define GB_TIMER_TEST_PHASE_PROBE_TIMEOUT_US 60000
#define GB_CLSACT_HANDLE 0xFFFF0000U

#if EAGAIN == EWOULDBLOCK
#define GB_ERRNO_IS_WOULDBLOCK(err) ((err) == EAGAIN)
#else
#define GB_ERRNO_IS_WOULDBLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#endif

enum gb_filter_kind {
    GB_FILTER_NONE = 0,
    GB_FILTER_FLOWER = 1,
    GB_FILTER_MATCHALL = 2,
};

struct gb_probe_socket {
    int rx_fd;
    int tx_fd;
    struct sockaddr_in dst;
};

static int64_t gb_now_tai_ns(void) {
    struct timespec ts;
    int64_t sec_ns;

    if (clock_gettime(CLOCK_TAI, &ts) < 0)
        return (int64_t)(-errno);

    sec_ns = (int64_t)ts.tv_sec * 1000000000LL;
    return sec_ns + (int64_t)ts.tv_nsec;
}

static int64_t gb_now_clock_ns(clockid_t clockid) {
    struct timespec ts;
    int64_t sec_ns;

    if (clock_gettime(clockid, &ts) < 0)
        return (int64_t)(-errno);

    sec_ns = (int64_t)ts.tv_sec * 1000000000LL;
    return sec_ns + (int64_t)ts.tv_nsec;
}

static int gb_sleep_ms(unsigned int ms) {
    struct timespec req;

    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000u);

    while (nanosleep(&req, &req) < 0) {
        if (errno == EINTR)
            continue;
        return -errno;
    }

    return 0;
}

static int gb_sleep_until_tai_ns(int64_t target_ns, unsigned int poll_ms) {
    int64_t now_ns;
    int ret;

    if (poll_ms == 0u)
        return -EINVAL;

    for (;;) {
        now_ns = gb_now_tai_ns();
        if (now_ns < 0)
            return (int)now_ns;
        if (now_ns >= target_ns)
            return 0;

        ret = gb_sleep_ms(poll_ms);
        if (ret < 0)
            return ret;
    }
}

static void gb_probe_close(struct gb_probe_socket* probe) {
    if (!probe)
        return;

    if (probe->rx_fd >= 0) {
        close(probe->rx_fd);
        probe->rx_fd = -1;
    }
    if (probe->tx_fd >= 0) {
        close(probe->tx_fd);
        probe->tx_fd = -1;
    }
}

static int gb_probe_set_timeout_us(int fd, suseconds_t timeout_us) {
    struct timeval tv;

    tv.tv_sec = 0;
    tv.tv_usec = timeout_us;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -errno;

    return 0;
}

static int gb_probe_open_port(struct gb_probe_socket* probe, uint16_t port) {
    struct sockaddr_in bind_addr;
    int fd_rx;
    int fd_tx;
    int ret;

    if (!probe)
        return -EINVAL;

    memset(probe, 0, sizeof(*probe));
    probe->rx_fd = -1;
    probe->tx_fd = -1;

    fd_rx = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_rx < 0)
        return -errno;

    ret = gb_probe_set_timeout_us(fd_rx, GB_TIMER_TEST_PROBE_TIMEOUT_US);
    if (ret < 0) {
        close(fd_rx);
        return ret;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd_rx, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        ret = -errno;
        close(fd_rx);
        return ret;
    }

    fd_tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_tx < 0) {
        ret = -errno;
        close(fd_rx);
        return ret;
    }

    probe->rx_fd = fd_rx;
    probe->tx_fd = fd_tx;

    memset(&probe->dst, 0, sizeof(probe->dst));
    probe->dst.sin_family = AF_INET;
    probe->dst.sin_port = htons(port);
    probe->dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    return 0;
}

static int gb_probe_open_auto(struct gb_probe_socket* probe, uint32_t base_index, uint16_t* port_out) {
    uint32_t seed;

    if (!probe || !port_out)
        return -EINVAL;

    seed = 30000u + (base_index % 20000u);
    for (uint32_t i = 0; i < 128u; i++) {
        uint16_t port = (uint16_t)(seed + i);
        int ret = gb_probe_open_port(probe, port);

        if (ret == 0) {
            *port_out = port;
            return 0;
        }
        if (ret != -EADDRINUSE)
            return ret;
    }

    return -EADDRINUSE;
}

static void gb_probe_drain(const struct gb_probe_socket* probe) {
    char buf[64];
    ssize_t n;

    if (!probe || probe->rx_fd < 0)
        return;

    for (;;) {
        n = recv(probe->rx_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0)
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
}

static int gb_probe_send_and_check(struct gb_probe_socket* probe, uint8_t marker, bool* delivered_out) {
    uint8_t rx_buf[8];
    ssize_t n;
    socklen_t dst_len;

    if (!probe || !delivered_out)
        return -EINVAL;
    if (probe->rx_fd < 0 || probe->tx_fd < 0)
        return -EINVAL;

    gb_probe_drain(probe);

    dst_len = (socklen_t)sizeof(probe->dst);
    n = sendto(probe->tx_fd, &marker, sizeof(marker), 0, (const struct sockaddr*)&probe->dst, dst_len);
    if (n < 0)
        return -errno;
    if (n != (ssize_t)sizeof(marker))
        return -EIO;

    for (;;) {
        n = recv(probe->rx_fd, rx_buf, sizeof(rx_buf), 0);
        if (n > 0) {
            *delivered_out = true;
            return 0;
        }
        if (n == 0) {
            *delivered_out = false;
            return 0;
        }
        if (errno == EINTR)
            continue;
        if (GB_ERRNO_IS_WOULDBLOCK(errno)) {
            *delivered_out = false;
            return 0;
        }
        return -errno;
    }
}

static uint32_t gb_filter_tcm_info(uint32_t prio, uint16_t protocol) {
    return TC_H_MAKE((prio & 0xFFFFu) << 16, protocol);
}

static int gb_qdisc_add_clsact(struct gb_nl_sock* sock,
                               struct gb_nl_msg* msg,
                               struct gb_nl_msg* resp,
                               int ifindex,
                               bool* created_out) {
    struct nlmsghdr* nlh;
    struct tcmsg* tcm;
    int ret;

    if (!sock || !msg || !resp || ifindex <= 0)
        return -EINVAL;

    if (created_out)
        *created_out = false;

    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWQDISC;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nlh->nlmsg_seq = 0;

    tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
    memset(tcm, 0, sizeof(*tcm));
    tcm->tcm_family = AF_UNSPEC;
    tcm->tcm_ifindex = ifindex;
    tcm->tcm_parent = TC_H_CLSACT;
    tcm->tcm_handle = GB_CLSACT_HANDLE;

    mnl_attr_put_strz(nlh, TCA_KIND, "clsact");

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret == -EEXIST)
        return 0;
    if (ret == 0 && created_out)
        *created_out = true;
    return ret;
}

static int gb_qdisc_del_clsact(struct gb_nl_sock* sock, struct gb_nl_msg* msg, struct gb_nl_msg* resp, int ifindex) {
    struct nlmsghdr* nlh;
    struct tcmsg* tcm;
    int ret;

    if (!sock || !msg || !resp || ifindex <= 0)
        return -EINVAL;

    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_DELQDISC;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = 0;

    tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
    memset(tcm, 0, sizeof(*tcm));
    tcm->tcm_family = AF_UNSPEC;
    tcm->tcm_ifindex = ifindex;
    tcm->tcm_parent = TC_H_CLSACT;
    tcm->tcm_handle = GB_CLSACT_HANDLE;

    mnl_attr_put_strz(nlh, TCA_KIND, "clsact");

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret == -ENOENT)
        return 0;
    return ret;
}

static void gb_filter_add_gate_action_ref(struct nlmsghdr* nlh, uint16_t act_attr, uint32_t gate_index) {
    struct nlattr *acts, *act, *act_opts, *entry_list;
    struct tc_gate gate_params;

    acts = mnl_attr_nest_start(nlh, act_attr);
    act = mnl_attr_nest_start(nlh, 1);

    mnl_attr_put_strz(nlh, TCA_ACT_KIND, "gate");

    act_opts = mnl_attr_nest_start(nlh, TCA_ACT_OPTIONS);
    memset(&gate_params, 0, sizeof(gate_params));
    gate_params.index = gate_index;
    gate_params.action = TC_ACT_PIPE;

    mnl_attr_put(nlh, TCA_GATE_PARMS, sizeof(gate_params), &gate_params);

    /*
     * Mirror tc/iproute2 "action gate index X" shape:
     * include an empty entry-list nest when referencing an existing action.
     */
    entry_list = mnl_attr_nest_start(nlh, TCA_GATE_ENTRY_LIST);
    mnl_attr_nest_end(nlh, entry_list);

    mnl_attr_nest_end(nlh, act_opts);
    mnl_attr_nest_end(nlh, act);
    mnl_attr_nest_end(nlh, acts);
}

static int gb_filter_add_gate(struct gb_nl_sock* sock,
                              struct gb_nl_msg* msg,
                              struct gb_nl_msg* resp,
                              enum gb_filter_kind kind,
                              int ifindex,
                              uint32_t filter_prio,
                              uint32_t filter_handle,
                              uint16_t probe_port,
                              uint32_t gate_index) {
    struct nlmsghdr* nlh;
    struct tcmsg* tcm;
    struct nlattr* opts;
    uint16_t proto;

    if (!sock || !msg || !resp || ifindex <= 0)
        return -EINVAL;

    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_NEWTFILTER;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nlh->nlmsg_seq = 0;

    tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
    memset(tcm, 0, sizeof(*tcm));
    tcm->tcm_family = AF_UNSPEC;
    tcm->tcm_ifindex = ifindex;
    tcm->tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_EGRESS);
    tcm->tcm_handle = filter_handle;

    switch (kind) {
        case GB_FILTER_FLOWER: {
            uint8_t ip_proto = IPPROTO_UDP;
            uint16_t port_be = htons(probe_port);

            proto = htons((uint16_t)ETH_P_IP);
            tcm->tcm_info = gb_filter_tcm_info(filter_prio, proto);
            mnl_attr_put_strz(nlh, TCA_KIND, "flower");

            opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);
            mnl_attr_put_u16(nlh, TCA_FLOWER_KEY_ETH_TYPE, proto);
            mnl_attr_put(nlh, TCA_FLOWER_KEY_IP_PROTO, sizeof(ip_proto), &ip_proto);
            mnl_attr_put_u16(nlh, TCA_FLOWER_KEY_UDP_DST, port_be);
            mnl_attr_put_u16(nlh, TCA_FLOWER_KEY_UDP_DST_MASK, UINT16_MAX);
            gb_filter_add_gate_action_ref(nlh, TCA_FLOWER_ACT, gate_index);
            mnl_attr_nest_end(nlh, opts);
            break;
        }
        case GB_FILTER_MATCHALL:
            proto = htons((uint16_t)ETH_P_ALL);
            tcm->tcm_info = gb_filter_tcm_info(filter_prio, proto);
            mnl_attr_put_strz(nlh, TCA_KIND, "matchall");

            opts = mnl_attr_nest_start(nlh, TCA_OPTIONS);
            gb_filter_add_gate_action_ref(nlh, TCA_MATCHALL_ACT, gate_index);
            mnl_attr_nest_end(nlh, opts);
            break;
        case GB_FILTER_NONE:
        default:
            return -EINVAL;
    }

    msg->len = nlh->nlmsg_len;
    return gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
}

static int gb_filter_del_gate(struct gb_nl_sock* sock,
                              struct gb_nl_msg* msg,
                              struct gb_nl_msg* resp,
                              enum gb_filter_kind kind,
                              int ifindex,
                              uint32_t filter_prio,
                              uint32_t filter_handle) {
    struct nlmsghdr* nlh;
    struct tcmsg* tcm;
    uint16_t proto;
    int ret;

    if (!sock || !msg || !resp || ifindex <= 0)
        return -EINVAL;

    gb_nl_msg_reset(msg);
    nlh = mnl_nlmsg_put_header(msg->buf);
    nlh->nlmsg_type = RTM_DELTFILTER;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = 0;

    tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
    memset(tcm, 0, sizeof(*tcm));
    tcm->tcm_family = AF_UNSPEC;
    tcm->tcm_ifindex = ifindex;
    tcm->tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_EGRESS);
    tcm->tcm_handle = filter_handle;

    switch (kind) {
        case GB_FILTER_FLOWER:
            proto = htons((uint16_t)ETH_P_IP);
            tcm->tcm_info = gb_filter_tcm_info(filter_prio, proto);
            mnl_attr_put_strz(nlh, TCA_KIND, "flower");
            break;
        case GB_FILTER_MATCHALL:
            proto = htons((uint16_t)ETH_P_ALL);
            tcm->tcm_info = gb_filter_tcm_info(filter_prio, proto);
            mnl_attr_put_strz(nlh, TCA_KIND, "matchall");
            break;
        case GB_FILTER_NONE:
        default:
            return -EINVAL;
    }

    msg->len = nlh->nlmsg_len;
    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret == -ENOENT)
        return 0;
    return ret;
}

static bool gb_can_fallback_to_matchall(int err) {
    return err == -ENOENT || err == -EOPNOTSUPP || err == -EINVAL;
}

int gb_selftest_gate_timer_start_logic(struct gb_nl_sock* sock, uint32_t base_index) {
    struct gb_nl_msg* msg = NULL;
    struct gb_nl_msg* resp = NULL;
    struct gb_probe_socket probe;
    struct gate_shape initial_shape;
    struct gate_shape replace_shape;
    struct gate_shape phase_shape;
    struct gate_entry open_entry;
    struct gate_entry closed_entry;
    struct gate_entry phase_entries[2];
    enum gb_filter_kind filter_kind = GB_FILTER_NONE;
    unsigned int ifindex_u;
    int ifindex;
    uint32_t filter_prio;
    uint32_t filter_handle;
    uint16_t probe_port = 0;
    int64_t now_ns;
    int64_t pre_base_deadline_ns;
    int64_t post_base_check_ns;
    int64_t phase_check_ns;
    int64_t phase_now_ns;
    uint8_t probe_marker = 0x21;
    bool qdisc_created = false;
    bool probe_delivered = false;
    bool initial_delivered = false;
    bool late_delivered = true;
    bool phase_delivered = false;
    uint32_t attempt;
    int ret;
    int test_ret = 0;

    if (!sock)
        return -EINVAL;

    memset(&probe, 0, sizeof(probe));
    probe.rx_fd = -1;
    probe.tx_fd = -1;

    ret = gb_selftest_alloc_msgs(&msg, &resp, (size_t)MNL_SOCKET_BUFFER_SIZE);
    if (ret < 0)
        return ret;

    ifindex_u = if_nametoindex("lo");
    if (ifindex_u == 0u) {
        test_ret = -errno;
        gb_selftest_log("failed to resolve loopback ifindex: %d\n", test_ret);
        goto out;
    }
    if (ifindex_u > (unsigned int)INT_MAX) {
        test_ret = -ERANGE;
        goto out;
    }
    ifindex = (int)ifindex_u;

    filter_prio = (base_index & 0xFFFFu);
    if (filter_prio == 0u)
        filter_prio = 1u;
    filter_handle = base_index == 0u ? 1u : base_index;

    ret = gb_probe_open_auto(&probe, base_index, &probe_port);
    if (ret < 0) {
        gb_selftest_log("failed to open probe socket on loopback: %d\n", ret);
        test_ret = ret;
        goto out;
    }

    /* Clean leftovers from interrupted runs that used the same index. */
    (void)gb_filter_del_gate(sock, msg, resp, GB_FILTER_FLOWER, ifindex, filter_prio, filter_handle);
    (void)gb_filter_del_gate(sock, msg, resp, GB_FILTER_MATCHALL, ifindex, filter_prio, filter_handle);
    gb_selftest_cleanup_gate(sock, msg, resp, base_index);

    ret = gb_qdisc_add_clsact(sock, msg, resp, ifindex, &qdisc_created);
    if (ret < 0) {
        gb_selftest_log("failed to add clsact on lo: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    gb_selftest_shape_default(&initial_shape, 1);
    initial_shape.base_time = 0;
    initial_shape.cycle_time = GB_TIMER_TEST_INITIAL_CYCLE_NS;

    gb_selftest_entry_default(&open_entry);
    open_entry.gate_state = true;
    open_entry.interval = (uint32_t)GB_TIMER_TEST_INITIAL_CYCLE_NS;

    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &initial_shape, &open_entry, 1, NLM_F_CREATE | NLM_F_EXCL, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("failed to create gate action: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_filter_add_gate(sock, msg, resp, GB_FILTER_FLOWER, ifindex, filter_prio, filter_handle, probe_port,
                             base_index);
    if (ret == 0) {
        filter_kind = GB_FILTER_FLOWER;
        gb_selftest_log("attached gate ref via flower on lo egress (port=%u)\n", (unsigned int)probe_port);
    }
    else if (gb_can_fallback_to_matchall(ret)) {
        gb_selftest_log("flower ref attach failed (%d), falling back to matchall\n", ret);
        ret = gb_filter_add_gate(sock, msg, resp, GB_FILTER_MATCHALL, ifindex, filter_prio, filter_handle, probe_port,
                                 base_index);
        if (ret < 0) {
            gb_selftest_log("failed to attach gate ref filter (matchall fallback): %d\n", ret);
            test_ret = ret;
            goto cleanup;
        }
        filter_kind = GB_FILTER_MATCHALL;
    }
    else {
        gb_selftest_log("failed to attach gate filter: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_probe_send_and_check(&probe, probe_marker++, &initial_delivered);
    if (ret < 0) {
        gb_selftest_log("initial probe send/check failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }
    if (!initial_delivered) {
        gb_selftest_log("initial probe unexpectedly dropped before replace\n");
        test_ret = -EINVAL;
        goto cleanup;
    }

    ret = gb_sleep_ms(GB_TIMER_TEST_PRE_REPLACE_WAIT_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    /*
     * Stress the cancel/restart path before final validation.
     * If stale inactive expires can clamp start, this loop should amplify it.
     */
    for (attempt = 0; attempt < GB_TIMER_TEST_TRIGGER_ATTEMPTS; attempt++) {
        clockid_t replace_clockid = (attempt & 1u) ? CLOCK_MONOTONIC : CLOCK_TAI;
        int64_t replace_now = gb_now_clock_ns(replace_clockid);

        if (replace_now < 0) {
            test_ret = (int)replace_now;
            gb_selftest_log("failed to read replace clock %d: %d\n", (int)replace_clockid, test_ret);
            goto cleanup;
        }

        gb_selftest_shape_default(&replace_shape, 1);
        replace_shape.clockid = (uint32_t)replace_clockid;
        replace_shape.base_time = (uint64_t)(replace_now + (int64_t)GB_TIMER_TEST_TRIGGER_REPLACE_DELAY_NS);
        replace_shape.cycle_time = GB_TIMER_TEST_REPLACE_CYCLE_NS;

        gb_selftest_entry_default(&closed_entry);
        closed_entry.gate_state = false;
        closed_entry.interval = (uint32_t)GB_TIMER_TEST_REPLACE_CYCLE_NS;

        gb_nl_msg_reset(msg);
        ret = build_gate_newaction(msg, base_index, &replace_shape, &closed_entry, 1, NLM_F_REPLACE, 0, -1);
        if (ret < 0) {
            test_ret = ret;
            goto cleanup;
        }

        ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
        if (ret < 0) {
            gb_selftest_log("replace gate action failed at attempt %u: %d\n", attempt, ret);
            test_ret = ret;
            goto cleanup;
        }

        ret = gb_probe_send_and_check(&probe, probe_marker++, &probe_delivered);
        if (ret < 0) {
            gb_selftest_log("attempt %u pre-base probe send/check failed: %d\n", attempt, ret);
            test_ret = ret;
            goto cleanup;
        }
        if (!probe_delivered) {
            gb_selftest_log("attempt %u dropped probe before new base_time (old kernel behavior)\n", attempt);
            test_ret = -EINVAL;
            goto cleanup;
        }

        ret = gb_sleep_ms(GB_TIMER_TEST_TRIGGER_STEP_SLEEP_MS);
        if (ret < 0) {
            test_ret = ret;
            goto cleanup;
        }
    }

    now_ns = gb_now_tai_ns();
    if (now_ns < 0) {
        test_ret = (int)now_ns;
        gb_selftest_log("failed to read CLOCK_TAI: %d\n", test_ret);
        goto cleanup;
    }

    gb_selftest_shape_default(&replace_shape, 1);
    replace_shape.clockid = CLOCK_TAI;
    replace_shape.base_time = (uint64_t)(now_ns + (int64_t)GB_TIMER_TEST_FINAL_REPLACE_DELAY_NS);
    replace_shape.cycle_time = GB_TIMER_TEST_REPLACE_CYCLE_NS;

    gb_selftest_entry_default(&closed_entry);
    closed_entry.gate_state = false;
    closed_entry.interval = (uint32_t)GB_TIMER_TEST_REPLACE_CYCLE_NS;

    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &replace_shape, &closed_entry, 1, NLM_F_REPLACE, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("replace gate action failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    pre_base_deadline_ns = (int64_t)replace_shape.base_time - (int64_t)GB_TIMER_TEST_PRE_BASE_GUARD_NS;
    now_ns = gb_now_tai_ns();
    if (now_ns < 0) {
        test_ret = (int)now_ns;
        goto cleanup;
    }
    if (pre_base_deadline_ns <= now_ns) {
        gb_selftest_log("insufficient pre-base window (now=%lld base=%llu)\n", (long long)now_ns,
                        (unsigned long long)replace_shape.base_time);
        test_ret = -ETIME;
        goto cleanup;
    }

    while (now_ns < pre_base_deadline_ns) {
        ret = gb_probe_send_and_check(&probe, probe_marker++, &probe_delivered);
        if (ret < 0) {
            gb_selftest_log("pre-base probe send/check failed: %d\n", ret);
            test_ret = ret;
            goto cleanup;
        }
        if (!probe_delivered) {
            gb_selftest_log("probe dropped before new base_time (old kernel behavior)\n");
            test_ret = -EINVAL;
            goto cleanup;
        }

        ret = gb_sleep_ms(GB_TIMER_TEST_PRE_BASE_POLL_MS);
        if (ret < 0) {
            test_ret = ret;
            goto cleanup;
        }
        now_ns = gb_now_tai_ns();
        if (now_ns < 0) {
            test_ret = (int)now_ns;
            goto cleanup;
        }
    }

    post_base_check_ns = (int64_t)replace_shape.base_time + (int64_t)GB_TIMER_TEST_POST_BASE_CONFIRM_NS;
    ret = gb_sleep_until_tai_ns(post_base_check_ns, GB_TIMER_TEST_POST_BASE_POLL_MS);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_probe_send_and_check(&probe, probe_marker++, &late_delivered);
    if (ret < 0) {
        gb_selftest_log("post-base probe send/check failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    if (late_delivered) {
        gb_selftest_log("late probe still delivered after new base_time\n");
        test_ret = -EINVAL;
        goto cleanup;
    }

    /*
     * Verify periodic gate transitions across multiple cycles. Probe each
     * open/closed window at a fixed offset from the scheduled boundary and
     * require bounded lateness versus target timestamps.
     */
    now_ns = gb_now_tai_ns();
    if (now_ns < 0) {
        test_ret = (int)now_ns;
        goto cleanup;
    }

    gb_selftest_shape_default(&phase_shape, 2);
    phase_shape.clockid = CLOCK_TAI;
    phase_shape.base_time = (uint64_t)(now_ns + (int64_t)GB_TIMER_TEST_PHASE_BASE_DELAY_NS);
    phase_shape.cycle_time = GB_TIMER_TEST_PHASE_CYCLE_NS;

    gb_selftest_entry_default(&phase_entries[0]);
    phase_entries[0].gate_state = true;
    phase_entries[0].interval = (uint32_t)GB_TIMER_TEST_PHASE_INTERVAL_NS;

    gb_selftest_entry_default(&phase_entries[1]);
    phase_entries[1].gate_state = false;
    phase_entries[1].interval = (uint32_t)GB_TIMER_TEST_PHASE_INTERVAL_NS;

    gb_nl_msg_reset(msg);
    ret = build_gate_newaction(msg, base_index, &phase_shape, phase_entries, 2, NLM_F_REPLACE, 0, -1);
    if (ret < 0) {
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_nl_send_recv(sock, msg, resp, GB_SELFTEST_TIMEOUT_MS);
    if (ret < 0) {
        gb_selftest_log("phase replace failed: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    ret = gb_probe_set_timeout_us(probe.rx_fd, GB_TIMER_TEST_PHASE_PROBE_TIMEOUT_US);
    if (ret < 0) {
        gb_selftest_log("failed to set phase probe timeout: %d\n", ret);
        test_ret = ret;
        goto cleanup;
    }

    for (uint32_t cycle = 0; cycle < GB_TIMER_TEST_PHASE_CYCLES; cycle++) {
        phase_check_ns = (int64_t)phase_shape.base_time + (int64_t)cycle * (int64_t)GB_TIMER_TEST_PHASE_CYCLE_NS +
                         (int64_t)GB_TIMER_TEST_PHASE_OFFSET_NS;
        ret = gb_sleep_until_tai_ns(phase_check_ns, GB_TIMER_TEST_PHASE_POLL_MS);
        if (ret < 0) {
            test_ret = ret;
            goto cleanup;
        }

        phase_now_ns = gb_now_tai_ns();
        if (phase_now_ns < 0) {
            test_ret = (int)phase_now_ns;
            goto cleanup;
        }
        if (phase_now_ns > phase_check_ns + (int64_t)GB_TIMER_TEST_PHASE_MAX_LATE_NS) {
            gb_selftest_log("phase open check too late: cycle=%u late=%lldns\n", cycle,
                            (long long)(phase_now_ns - phase_check_ns));
            test_ret = -ETIME;
            goto cleanup;
        }

        ret = gb_probe_send_and_check(&probe, probe_marker++, &phase_delivered);
        if (ret < 0) {
            gb_selftest_log("phase open probe failed: cycle=%u err=%d\n", cycle, ret);
            test_ret = ret;
            goto cleanup;
        }
        if (!phase_delivered) {
            gb_selftest_log("phase open probe dropped in open window: cycle=%u\n", cycle);
            test_ret = -EINVAL;
            goto cleanup;
        }

        phase_check_ns = (int64_t)phase_shape.base_time + (int64_t)cycle * (int64_t)GB_TIMER_TEST_PHASE_CYCLE_NS +
                         (int64_t)GB_TIMER_TEST_PHASE_INTERVAL_NS + (int64_t)GB_TIMER_TEST_PHASE_OFFSET_NS;
        ret = gb_sleep_until_tai_ns(phase_check_ns, GB_TIMER_TEST_PHASE_POLL_MS);
        if (ret < 0) {
            test_ret = ret;
            goto cleanup;
        }

        phase_now_ns = gb_now_tai_ns();
        if (phase_now_ns < 0) {
            test_ret = (int)phase_now_ns;
            goto cleanup;
        }
        if (phase_now_ns > phase_check_ns + (int64_t)GB_TIMER_TEST_PHASE_MAX_LATE_NS) {
            gb_selftest_log("phase closed check too late: cycle=%u late=%lldns\n", cycle,
                            (long long)(phase_now_ns - phase_check_ns));
            test_ret = -ETIME;
            goto cleanup;
        }

        ret = gb_probe_send_and_check(&probe, probe_marker++, &phase_delivered);
        if (ret < 0) {
            gb_selftest_log("phase closed probe failed: cycle=%u err=%d\n", cycle, ret);
            test_ret = ret;
            goto cleanup;
        }
        if (phase_delivered) {
            gb_selftest_log("phase closed probe delivered in closed window: cycle=%u\n", cycle);
            test_ret = -EINVAL;
            goto cleanup;
        }
    }

    (void)gb_probe_set_timeout_us(probe.rx_fd, GB_TIMER_TEST_PROBE_TIMEOUT_US);

cleanup:
    if (filter_kind != GB_FILTER_NONE)
        (void)gb_filter_del_gate(sock, msg, resp, filter_kind, ifindex, filter_prio, filter_handle);

    if (qdisc_created)
        (void)gb_qdisc_del_clsact(sock, msg, resp, ifindex);

    gb_selftest_cleanup_gate(sock, msg, resp, base_index);
    gb_probe_close(&probe);

out:
    gb_selftest_free_msgs(msg, resp);
    return test_ret;
}
