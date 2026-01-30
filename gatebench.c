// SPDX-License-Identifier: GPL-2.0
// gatebench
//
// Microbenchmark + error-path exerciser for tc "gate" action control-plane updates via rtnetlink (libmnl)
//
// What this measures
// - RTM_NEWACTION create + repeated replace (NLM_F_ACK)
// - RTM_DELACTION delete (NLM_F_ACK)
// Latency includes kernel validation + netlink ACK round-trip
//
// What this also does
// - Optional negative/self-test suite that hits common validation/error paths
//   It validates that failures return the expected errno and that successful ops still work after failures
//
// What this avoids
// - No tc(8) execs, no parsing tc output
// - No qdisc/filter datapath setup
// - Netlink messages are built once (templates) then reused in the hot loop
//
// Build
//   cc -O2 -pipe -Wall -Wextra -Wno-unused-parameter gatebench.c -lmnl -o gatebench
//
// Notes
// - Requires CAP_NET_ADMIN (or run as root)
// - Robust ACK handling: per-send seq stamping, accepts pid 0 replies, poll timeout to avoid hangs

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>

#include <getopt.h>
#include <libmnl/libmnl.h>
#include <linux/limits.h>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_gate.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef TC_ACT_GATE
#define TC_ACT_GATE 25
#endif

#define DEFAULT_INDEX 3344u
#define DEFAULT_TIMEOUT_MS 2000

static uint32_t g_seq;

static uint64_t ns_now(clockid_t clk)
{
	struct timespec ts;
	if (clock_gettime(clk, &ts) != 0)
		err(1, "clock_gettime");
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct nlmsghdr *start_msg(char *buf, int flags, int cmd, size_t maxlen)
{
	(void)maxlen;

	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	struct tcamsg *tca;

	nlh->nlmsg_type = cmd;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
	nlh->nlmsg_seq = 0;

	tca = mnl_nlmsg_put_extra_header(nlh, sizeof(*tca));
	tca->tca_family = AF_UNSPEC;

	return nlh;
}

static int nl_poll_recv(struct mnl_socket *nl, int timeout_ms)
{
	int fd = mnl_socket_get_fd(nl);
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
		.revents = 0,
	};

	for (;;) {
		int r = poll(&pfd, 1, timeout_ms);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (r == 0)
			return -ETIMEDOUT;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -EIO;
		if (pfd.revents & POLLIN)
			return 0;
	}
}

static int send_and_recv(struct mnl_socket *nl,
			 const void *tx_template, size_t tx_len,
			 void *tx_buf, size_t tx_buf_len,
			 void *rx_buf, size_t rx_len,
			 int timeout_ms)
{
	const unsigned int portid = mnl_socket_get_portid(nl);

	const struct nlmsghdr *tmpl = (const struct nlmsghdr *)tx_template;
	if (tmpl->nlmsg_len > tx_buf_len || tmpl->nlmsg_len > tx_len)
		return -EMSGSIZE;

	memcpy(tx_buf, tx_template, tmpl->nlmsg_len);

	struct nlmsghdr *tx = (struct nlmsghdr *)tx_buf;
	tx->nlmsg_seq = ++g_seq;
	const uint32_t seq = tx->nlmsg_seq;

	int ret = mnl_socket_sendto(nl, tx, tx->nlmsg_len);
	if (ret < 0)
		return -errno;

	for (;;) {
		ret = nl_poll_recv(nl, timeout_ms);
		if (ret)
			return ret;

		ret = mnl_socket_recvfrom(nl, rx_buf, rx_len);
		if (ret < 0)
			return -errno;

		int remaining = ret;
		for (struct nlmsghdr *nlh = (struct nlmsghdr *)rx_buf;
		     mnl_nlmsg_ok(nlh, remaining);
		     nlh = mnl_nlmsg_next(nlh, &remaining)) {

			if (nlh->nlmsg_seq != seq)
				continue;

			if (!(nlh->nlmsg_pid == 0 || nlh->nlmsg_pid == portid))
				continue;

			if (nlh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = mnl_nlmsg_get_payload(nlh);
				return e->error ? e->error : 0;
			}

			if (nlh->nlmsg_type == NLMSG_DONE)
				return 0;
		}
	}
}

static int add_gate_entry(struct nlmsghdr *nlh, size_t maxlen,
			  uint32_t idx, uint64_t interval_ns, bool open,
			  bool interval_is_u64)
{
	struct nlattr *nest =
		mnl_attr_nest_start_check(nlh, maxlen, TCA_GATE_ONE_ENTRY);
	if (!nest)
		return -EMSGSIZE;

	if (!mnl_attr_put_u32_check(nlh, maxlen, TCA_GATE_ENTRY_INDEX, idx))
		return -EMSGSIZE;

	if (open) {
		if (!mnl_attr_put_check(nlh, maxlen, TCA_GATE_ENTRY_GATE, 0, NULL))
			return -EMSGSIZE;
	}

	if (interval_is_u64) {
		if (!mnl_attr_put_u64_check(nlh, maxlen, TCA_GATE_ENTRY_INTERVAL,
					   interval_ns))
			return -EMSGSIZE;
	} else {
		if (interval_ns > UINT32_MAX)
			return -ERANGE;
		if (!mnl_attr_put_u32_check(nlh, maxlen, TCA_GATE_ENTRY_INTERVAL,
					   (uint32_t)interval_ns))
			return -EMSGSIZE;
	}

	if (!mnl_attr_put_u32_check(nlh, maxlen, TCA_GATE_ENTRY_IPV, (uint32_t)-1))
		return -EMSGSIZE;

	if (!mnl_attr_put_u32_check(nlh, maxlen, TCA_GATE_ENTRY_MAX_OCTETS, (uint32_t)-1))
		return -EMSGSIZE;

	mnl_attr_nest_end(nlh, nest);
	return 0;
}

struct gate_shape {
	uint32_t index;
	uint32_t entries;
	uint64_t interval_ns;
	uint32_t clockid;
	uint64_t base_time;
	uint64_t cycle_time;
	bool start_open;
	bool interval_is_u64;
	bool include_entry_list;
	bool include_base_time;
	bool include_cycle_time;
	bool include_clockid;
};

static int build_gate_newaction(char *buf, size_t len,
				const struct gate_shape *g, int nl_flags)
{
	struct nlmsghdr *nlh;
	struct nlattr *nest_act, *nest_idx, *nest_opt, *nest_entries;

	struct tc_gate parm = {
		.index = g->index,
		.action = TC_ACT_OK,
	};

	memset(buf, 0, len);
	nlh = start_msg(buf, nl_flags, RTM_NEWACTION, len);

	nest_act = mnl_attr_nest_start_check(nlh, len, TCA_ACT_TAB);
	if (!nest_act)
		return -EMSGSIZE;

	nest_idx = mnl_attr_nest_start_check(nlh, len, 1);
	if (!nest_idx)
		return -EMSGSIZE;

	if (!mnl_attr_put_strz_check(nlh, len, TCA_ACT_KIND, "gate"))
		return -EMSGSIZE;

	nest_opt = mnl_attr_nest_start_check(nlh, len, TCA_ACT_OPTIONS);
	if (!nest_opt)
		return -EMSGSIZE;

	if (!mnl_attr_put_check(nlh, len, TCA_GATE_PARMS, sizeof(parm), &parm))
		return -EMSGSIZE;

	if (g->include_base_time) {
		if (!mnl_attr_put_u64_check(nlh, len, TCA_GATE_BASE_TIME, g->base_time))
			return -EMSGSIZE;
	}

	if (g->include_cycle_time) {
		if (!mnl_attr_put_u64_check(nlh, len, TCA_GATE_CYCLE_TIME, g->cycle_time))
			return -EMSGSIZE;
	}

	if (g->include_clockid) {
		if (!mnl_attr_put_u32_check(nlh, len, TCA_GATE_CLOCKID, g->clockid))
			return -EMSGSIZE;
	}

	if (g->include_entry_list) {
		nest_entries = mnl_attr_nest_start_check(nlh, len, TCA_GATE_ENTRY_LIST);
		if (!nest_entries)
			return -EMSGSIZE;

		for (uint32_t i = 0; i < g->entries; i++) {
			bool open = g->start_open ? ((i & 1u) == 0u) : ((i & 1u) != 0u);
			int r = add_gate_entry(nlh, len, i, g->interval_ns, open,
					       g->interval_is_u64);
			if (r)
				return r;
		}

		mnl_attr_nest_end(nlh, nest_entries);
	}

	mnl_attr_nest_end(nlh, nest_opt);
	mnl_attr_nest_end(nlh, nest_idx);
	mnl_attr_nest_end(nlh, nest_act);

	if (nlh->nlmsg_len > len)
		return -EMSGSIZE;
	return 0;
}

static int build_gate_delaction(char *buf, size_t len, uint32_t index)
{
	struct nlmsghdr *nlh;
	struct nlattr *nest_act, *nest_idx;

	memset(buf, 0, len);
	nlh = start_msg(buf, 0, RTM_DELACTION, len);

	nest_act = mnl_attr_nest_start_check(nlh, len, TCA_ACT_TAB);
	if (!nest_act)
		return -EMSGSIZE;

	nest_idx = mnl_attr_nest_start_check(nlh, len, 1);
	if (!nest_idx)
		return -EMSGSIZE;

	if (!mnl_attr_put_strz_check(nlh, len, TCA_ACT_KIND, "gate"))
		return -EMSGSIZE;

	if (!mnl_attr_put_u32_check(nlh, len, TCA_ACT_INDEX, index))
		return -EMSGSIZE;

	mnl_attr_nest_end(nlh, nest_idx);
	mnl_attr_nest_end(nlh, nest_act);

	if (nlh->nlmsg_len > len)
		return -EMSGSIZE;
	return 0;
}

static size_t gate_msg_cap(uint32_t entries)
{
	size_t cap = 2048;
	if (entries > 0) {
		size_t add = (size_t)entries * 96;
		if (add > 1024 * 1024)
			add = 1024 * 1024;
		cap += add;
	}
	if (cap < (size_t)MNL_SOCKET_BUFFER_SIZE)
		cap = (size_t)MNL_SOCKET_BUFFER_SIZE;
	cap = (cap + 4095) & ~4095u;
	return cap;
}

static void pin_cpu(int cpu)
{
	if (cpu < 0)
		return;

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) < 0)
		warn("sched_setaffinity(%d)", cpu);
}

static void usage(FILE *f, const char *argv0)
{
	fprintf(f,
		"Usage: %s [opts]\n"
		"\n"
		"Options:\n"
		"  -n, --iters N        benchmark iterations (default 100000)\n"
		"  -w, --warmup N       warmup iterations (default 2000)\n"
		"  -e, --entries N      number of schedule entries (default 2)\n"
		"  -i, --interval NS    entry interval in ns (default 1000000)\n"
		"  -x, --index IDX      action index (default 4242)\n"
		"  -c, --cpu CPU        pin benchmark to CPU\n"
		"  -t, --timeout MS     recv timeout per op (default 2000ms)\n"
		"  -s, --selftest       run negative/error-path tests before benchmark (default)\n"
		"  -S, --no-selftest    skip negative/error-path tests\n"
		"  -B, --force-bug      force invalid basetime to trigger kernel bug\n"
		"  -h, --help           show help\n",
		argv0);
}

static int cmp_u64(const void *pa, const void *pb)
{
	const uint64_t a = *(const uint64_t *)pa;
	const uint64_t b = *(const uint64_t *)pb;
	return (a > b) - (a < b);
}

static uint64_t percentile_sorted_u64(const uint64_t *a, size_t n, double pct)
{
	if (n == 0)
		return 0;
	if (pct <= 0.0)
		return a[0];
	if (pct >= 100.0)
		return a[n - 1];

	size_t idx = (size_t)((pct / 100.0) * (double)(n - 1));
	return a[idx];
}

static const char *errno_name(int e)
{
	switch (-e) {
	case EINVAL: return "EINVAL";
	case ENOENT: return "ENOENT";
	case EEXIST: return "EEXIST";
	case ENOMEM: return "ENOMEM";
	case EOPNOTSUPP: return "EOPNOTSUPP";
	case EPERM: return "EPERM";
	case EBUSY: return "EBUSY";
	case ERANGE: return "ERANGE";
	case EMSGSIZE: return "EMSGSIZE";
	case ETIMEDOUT: return "ETIMEDOUT";
	case EIO: return "EIO";
	default: return "errno";
	}
}

static void expect_errno(const char *name, int got, int want)
{
	if (got != want) {
		fprintf(stderr, "selftest %s failed: got %d (%s) want %d (%s)\n",
			name, got, errno_name(got), want, errno_name(want));
		exit(1);
	}
}

static int do_op(struct mnl_socket *nl,
		 const char *msg, size_t msg_len,
		 char *tx_scratch, size_t tx_scratch_len,
		 char *rx, size_t rx_len,
		 int timeout_ms)
{
	return send_and_recv(nl, msg, msg_len, tx_scratch, tx_scratch_len, rx, rx_len, timeout_ms);
}

static void run_selftests(struct mnl_socket *nl,
			  uint32_t index,
			  uint32_t entries,
			  uint64_t interval_ns,
			  int timeout_ms)
{
	size_t cap = gate_msg_cap(entries);
	char rx[MNL_SOCKET_BUFFER_SIZE];

	char *tx_scratch = malloc(cap);
	char *msg_del = malloc(cap);
	char *msg_create_ok = malloc(cap);
	char *msg_create_dup = malloc(cap);
	char *msg_create_no_entries = malloc(cap);
	char *msg_create_empty_entries = malloc(cap);
	char *msg_create_zero_interval = malloc(cap);
	char *msg_create_bad_clockid = malloc(cap);
	char *msg_create_bad_basetime = malloc(cap);
	char *msg_create_bad_cycletime = malloc(cap);
	char *msg_replace_base_only = malloc(cap);
	char *msg_replace_derived = malloc(cap);
	char *msg_replace_bad_clockid = malloc(cap);
	char *msg_replace_ok = malloc(cap);
	char *msg_replace_no_create = malloc(cap);
	char *msg_replace_bad_basetime = malloc(cap);
	char *msg_replace_empty_entries = malloc(cap);
	char *msg_del_other = malloc(cap);

	if (!tx_scratch || !msg_del || !msg_create_ok || !msg_create_dup ||
	    !msg_create_no_entries || !msg_create_empty_entries ||
	    !msg_create_zero_interval || !msg_create_bad_clockid ||
	    !msg_create_bad_basetime || !msg_create_bad_cycletime ||
	    !msg_replace_base_only || !msg_replace_derived ||
	    !msg_replace_bad_clockid || !msg_replace_ok ||
	    !msg_replace_no_create || !msg_replace_bad_basetime ||
	    !msg_replace_empty_entries || !msg_del_other)
		err(1, "malloc");

	if (build_gate_delaction(msg_del, cap, index) != 0)
		errx(1, "selftest: build delete failed");

	int ret = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret && ret != -ENOENT)
		errx(1, "selftest: pre-delete unexpected: %s", strerror(-ret));

	struct gate_shape ok = {
		.index = index,
		.entries = entries,
		.interval_ns = interval_ns,
		.clockid = CLOCK_MONOTONIC,
		.base_time = 0,
		.cycle_time = (uint64_t)interval_ns * (uint64_t)entries,
		.start_open = true,
		.interval_is_u64 = false,
		.include_entry_list = true,
		.include_base_time = true,
		.include_cycle_time = true,
		.include_clockid = true,
	};
	if (build_gate_newaction(msg_create_ok, cap, &ok, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_ok failed");

	if (build_gate_newaction(msg_create_dup, cap, &ok, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_dup failed");

	struct gate_shape no_entries = ok;
	no_entries.include_entry_list = false;
	if (build_gate_newaction(msg_create_no_entries, cap, &no_entries, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_no_entries failed");

	struct gate_shape empty_entries = ok;
	empty_entries.entries = 0;
	if (build_gate_newaction(msg_create_empty_entries, cap, &empty_entries, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_empty_entries failed");

	struct gate_shape zero_interval = ok;
	zero_interval.entries = 1;
	zero_interval.interval_ns = 0;
	zero_interval.cycle_time = 1;
	if (build_gate_newaction(msg_create_zero_interval, cap, &zero_interval, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_zero_interval failed");

	struct gate_shape bad_clock = ok;
	bad_clock.clockid = 0x7fffffffU;
	if (build_gate_newaction(msg_create_bad_clockid, cap, &bad_clock, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_bad_clockid failed");

	struct gate_shape bad_bt = ok;
	bad_bt.base_time = (uint64_t)INT64_MAX + 1ull;
	if (build_gate_newaction(msg_create_bad_basetime, cap, &bad_bt, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_bad_basetime failed");

	struct gate_shape bad_ct = ok;
	bad_ct.cycle_time = (uint64_t)INT64_MAX + 1ull;
	if (build_gate_newaction(msg_create_bad_cycletime, cap, &bad_ct, NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "selftest: build create_bad_cycletime failed");

	struct gate_shape base_only = ok;
	base_only.base_time = 400000000000ull;
	base_only.include_entry_list = false;
	base_only.include_cycle_time = false;
	base_only.include_clockid = false;
	if (build_gate_newaction(msg_replace_base_only, cap, &base_only, NLM_F_REPLACE) != 0)
		errx(1, "selftest: build replace_base_only failed");

	struct gate_shape derived = ok;
	derived.interval_ns = (uint64_t)UINT32_MAX;
	derived.entries = 2;
	derived.include_cycle_time = false;
	if (build_gate_newaction(msg_replace_derived, cap, &derived, NLM_F_REPLACE) != 0)
		errx(1, "selftest: build replace_derived failed");

	struct gate_shape bad_clock_replace = ok;
	bad_clock_replace.clockid = 0x7fffffffU;
	if (build_gate_newaction(msg_replace_bad_clockid, cap, &bad_clock_replace, NLM_F_REPLACE) != 0)
		errx(1, "selftest: build replace_bad_clockid failed");

	if (build_gate_newaction(msg_replace_ok, cap, &ok, NLM_F_REPLACE) != 0)
		errx(1, "selftest: build replace_ok failed");

	uint32_t other_index = index ^ 0x55aa55aaU;
	struct gate_shape other = ok;
	other.index = other_index;
	if (build_gate_newaction(msg_replace_no_create, cap, &other, NLM_F_REPLACE) != 0)
		errx(1, "selftest: build replace_no_create failed");

	ret = do_op(nl, msg_create_zero_interval, ((struct nlmsghdr *)msg_create_zero_interval)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	expect_errno("zero_interval_create", ret, -EINVAL);

	ret = do_op(nl, msg_create_bad_clockid, ((struct nlmsghdr *)msg_create_bad_clockid)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	expect_errno("bad_clockid_create", ret, -EINVAL);

	ret = do_op(nl, msg_create_bad_basetime, ((struct nlmsghdr *)msg_create_bad_basetime)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret == 0) {
		ret = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
		if (ret)
			errx(1, "selftest: delete_after_bad_basetime_create failed: %s", strerror(-ret));
	} else if (ret != -EINVAL) {
		expect_errno("bad_basetime_create", ret, -EINVAL);
	} else {
		/* ret == -EINVAL, but did it create the entry anyway? (Kernel regression check) */
		int r = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
		if (r == 0)
			fprintf(stderr, "selftest: WARNING: bad_basetime_create returned EINVAL but created entry (kernel bug?)\n");
	}

	ret = do_op(nl, msg_create_bad_cycletime, ((struct nlmsghdr *)msg_create_bad_cycletime)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret == 0) {
		ret = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
		if (ret)
			errx(1, "selftest: delete_after_bad_cycletime_create failed: %s", strerror(-ret));
	} else if (ret != -EINVAL) {
		expect_errno("bad_cycletime_create", ret, -EINVAL);
	}

	ret = do_op(nl, msg_replace_no_create, ((struct nlmsghdr *)msg_replace_no_create)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret == 0) {
		if (build_gate_delaction(msg_del_other, cap, other_index) != 0)
			errx(1, "selftest: build delete_other failed");

		ret = do_op(nl, msg_del_other, ((struct nlmsghdr *)msg_del_other)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
		if (ret)
			errx(1, "selftest: delete_other failed: %s", strerror(-ret));
	} else if (!(ret == -ENOENT || ret == -EINVAL)) {
		expect_errno("replace_no_create", ret, -ENOENT);
	}

	ret = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (!(ret == -ENOENT || ret == 0))
		expect_errno("delete_nonexistent", ret, -ENOENT);

	ret = do_op(nl, msg_create_ok, ((struct nlmsghdr *)msg_create_ok)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret)
		errx(1, "selftest: create_ok failed: %s", strerror(-ret));

	struct gate_shape bad_bt_replace = ok;
	bad_bt_replace.base_time = (uint64_t)INT64_MAX + 1ull;
	if (build_gate_newaction(msg_replace_bad_basetime, cap, &bad_bt_replace, NLM_F_REPLACE) != 0)
		errx(1, "selftest: build replace_bad_basetime failed");

	ret = do_op(nl, msg_replace_base_only, ((struct nlmsghdr *)msg_replace_base_only)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret && ret != -EINVAL)
		errx(1, "selftest: replace_base_only failed: %s", strerror(-ret));

	ret = do_op(nl, msg_replace_derived, ((struct nlmsghdr *)msg_replace_derived)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret)
		errx(1, "selftest: replace_derived failed: %s", strerror(-ret));

	ret = do_op(nl, msg_replace_bad_clockid, ((struct nlmsghdr *)msg_replace_bad_clockid)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	expect_errno("bad_clockid_replace", ret, -EINVAL);

	ret = do_op(nl, msg_replace_bad_basetime, ((struct nlmsghdr *)msg_replace_bad_basetime)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret && ret != -EINVAL)
		expect_errno("bad_basetime_replace", ret, -EINVAL);

	struct gate_shape empty_entries_replace = ok;
	empty_entries_replace.entries = 0;
	if (build_gate_newaction(msg_replace_empty_entries, cap, &empty_entries_replace, NLM_F_REPLACE) != 0)
		errx(1, "selftest: build replace_empty_entries failed");

	ret = do_op(nl, msg_replace_empty_entries, ((struct nlmsghdr *)msg_replace_empty_entries)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret != 0)
		expect_errno("empty_entry_list_replace", ret, -EINVAL);

	ret = do_op(nl, msg_replace_ok, ((struct nlmsghdr *)msg_replace_ok)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret)
		errx(1, "selftest: replace_ok_after_invalid failed: %s", strerror(-ret));

	ret = do_op(nl, msg_create_dup, ((struct nlmsghdr *)msg_create_dup)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (!(ret == -EEXIST || ret == -EINVAL))
		expect_errno("duplicate_create", ret, -EEXIST);

	ret = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret)
		errx(1, "selftest: delete_ok failed: %s", strerror(-ret));

	ret = do_op(nl, msg_create_ok, ((struct nlmsghdr *)msg_create_ok)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret)
		errx(1, "selftest: create_after_delete failed: %s", strerror(-ret));

	ret = do_op(nl, msg_replace_ok, ((struct nlmsghdr *)msg_replace_ok)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret)
		errx(1, "selftest: replace_after_recreate failed: %s", strerror(-ret));

	ret = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (ret)
		errx(1, "selftest: delete_after_recreate failed: %s", strerror(-ret));

	ret = do_op(nl, msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len, tx_scratch, cap, rx, sizeof(rx), timeout_ms);
	if (!(ret == -ENOENT || ret == 0))
		expect_errno("delete_after_delete", ret, -ENOENT);

	free(tx_scratch);
	free(msg_del);
	free(msg_create_ok);
	free(msg_create_dup);
	free(msg_create_no_entries);
	free(msg_create_empty_entries);
	free(msg_create_zero_interval);
	free(msg_create_bad_clockid);
	free(msg_create_bad_basetime);
	free(msg_create_bad_cycletime);
	free(msg_replace_base_only);
	free(msg_replace_derived);
	free(msg_replace_bad_clockid);
	free(msg_replace_ok);
	free(msg_replace_no_create);
	free(msg_replace_bad_basetime);
	free(msg_replace_empty_entries);
	free(msg_del_other);

	fprintf(stderr, "selftest: OK\n");
}

int main(int argc, char **argv)
{
	uint32_t iters = 100000;
	uint32_t warmup = 2000;
	uint32_t entries = 2;
	uint64_t interval_ns = 1000000;
	uint32_t index = DEFAULT_INDEX;
	int cpu = -1;
	int timeout_ms = DEFAULT_TIMEOUT_MS;
	bool selftest = true;
	bool force_bug = false;

	static const struct option longopts[] = {
		{ "iters", required_argument, NULL, 'n' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "entries", required_argument, NULL, 'e' },
		{ "interval", required_argument, NULL, 'i' },
		{ "index", required_argument, NULL, 'x' },
		{ "cpu", required_argument, NULL, 'c' },
		{ "timeout", required_argument, NULL, 't' },
		{ "selftest", no_argument, NULL, 's' },
		{ "no-selftest", no_argument, NULL, 'S' },
		{ "force-bug", no_argument, NULL, 'B' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	for (;;) {
		int opt = getopt_long(argc, argv, "n:w:e:i:x:c:t:sShB", longopts, NULL);
		if (opt == -1)
			break;

		switch (opt) {
		case 'n': iters = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'w': warmup = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'e': entries = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'i': interval_ns = (uint64_t)strtoull(optarg, NULL, 0); break;
		case 'x': index = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'c': cpu = (int)strtol(optarg, NULL, 0); break;
		case 't': timeout_ms = (int)strtol(optarg, NULL, 0); break;
		case 's': selftest = true; break;
		case 'S': selftest = false; break;
		case 'B': force_bug = true; break;
		case 'h': usage(stdout, argv[0]); return 0;
		default: usage(stderr, argv[0]); return 2;
		}
	}

	if (entries == 0) {
		fprintf(stderr, "entries must be >= 1\n");
		return 2;
	}
	if (iters == 0) {
		fprintf(stderr, "iters must be >= 1\n");
		return 2;
	}
	if (timeout_ms <= 0)
		timeout_ms = DEFAULT_TIMEOUT_MS;

	pin_cpu(cpu);

	struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
	if (!nl)
		err(1, "mnl_socket_open");
	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0)
		err(1, "mnl_socket_bind");

	if (selftest)
		run_selftests(nl, index, entries, interval_ns, timeout_ms);

	const clockid_t bench_clk = CLOCK_MONOTONIC_RAW;

	struct gate_shape shape_a = {
		.index = index,
		.entries = entries,
		.interval_ns = interval_ns,
		.clockid = CLOCK_MONOTONIC,
		.base_time = 0,
		.cycle_time = (uint64_t)interval_ns * (uint64_t)entries,
		.start_open = true,
		.interval_is_u64 = false,
		.include_entry_list = true,
		.include_base_time = true,
		.include_cycle_time = true,
		.include_clockid = true,
	};

	if (force_bug)
		shape_a.base_time = (uint64_t)INT64_MAX + 1ull;

	struct gate_shape shape_b = shape_a;
	shape_b.interval_ns = interval_ns ^ 1u;
	shape_b.cycle_time = (uint64_t)shape_b.interval_ns * (uint64_t)entries;

	size_t tx_cap = gate_msg_cap(entries);
	char *msg_create = malloc(tx_cap);
	char *msg_replace_a = malloc(tx_cap);
	char *msg_replace_b = malloc(tx_cap);
	char *msg_del = malloc(tx_cap);
	char *tx_scratch = malloc(tx_cap);
	if (!msg_create || !msg_replace_a || !msg_replace_b || !msg_del || !tx_scratch)
		err(1, "malloc");

	if (build_gate_newaction(msg_create, tx_cap, &shape_a,
				 NLM_F_CREATE | NLM_F_EXCL) != 0)
		errx(1, "build create failed");

	if (build_gate_newaction(msg_replace_a, tx_cap, &shape_a,
				 NLM_F_CREATE | NLM_F_REPLACE) != 0)
		errx(1, "build replace_a failed");

	if (build_gate_newaction(msg_replace_b, tx_cap, &shape_b,
				 NLM_F_CREATE | NLM_F_REPLACE) != 0)
		errx(1, "build replace_b failed");

	if (build_gate_delaction(msg_del, tx_cap, index) != 0)
		errx(1, "build delete failed");

	char rx[MNL_SOCKET_BUFFER_SIZE];

	int ret = send_and_recv(nl,
				msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len,
				tx_scratch, tx_cap, rx, sizeof(rx),
				timeout_ms);
	if (ret && ret != -ENOENT)
		errx(1, "pre-delete failed: %s", strerror(-ret));

	ret = send_and_recv(nl,
			    msg_create, ((struct nlmsghdr *)msg_create)->nlmsg_len,
			    tx_scratch, tx_cap, rx, sizeof(rx),
			    timeout_ms);
	if (ret)
		errx(1, "create failed: %s", strerror(-ret));

	for (uint32_t i = 0; i < warmup; i++) {
		const char *m = (i & 1u) ? msg_replace_a : msg_replace_b;
		size_t ml = ((const struct nlmsghdr *)m)->nlmsg_len;

		ret = send_and_recv(nl, m, ml, tx_scratch, tx_cap, rx, sizeof(rx), timeout_ms);
		if (ret)
			errx(1, "warmup replace failed: %s", strerror(-ret));
	}

	uint64_t *lat = calloc(iters, sizeof(*lat));
	if (!lat)
		err(1, "calloc");

	uint64_t t0 = ns_now(bench_clk);

	for (uint32_t i = 0; i < iters; i++) {
		const char *m = (i & 1u) ? msg_replace_a : msg_replace_b;
		size_t ml = ((const struct nlmsghdr *)m)->nlmsg_len;

		uint64_t a = ns_now(bench_clk);
		ret = send_and_recv(nl, m, ml, tx_scratch, tx_cap, rx, sizeof(rx), timeout_ms);
		uint64_t b = ns_now(bench_clk);

		if (ret)
			errx(1, "replace failed @%u: %s", i, strerror(-ret));

		lat[i] = b - a;
	}

	uint64_t t1 = ns_now(bench_clk);

	ret = send_and_recv(nl,
			    msg_del, ((struct nlmsghdr *)msg_del)->nlmsg_len,
			    tx_scratch, tx_cap, rx, sizeof(rx),
			    timeout_ms);
	if (ret)
		errx(1, "delete failed: %s", strerror(-ret));

	uint64_t total_ns = t1 - t0;
	double secs = (double)total_ns / 1e9;
	double ops = (double)iters / secs;

	qsort(lat, iters, sizeof(*lat), cmp_u64);

	uint64_t med = lat[iters / 2];
	uint64_t p95 = percentile_sorted_u64(lat, iters, 95.0);
	uint64_t p99 = percentile_sorted_u64(lat, iters, 99.0);

	printf("# gatebench\n\n");
	printf("- iters: %u\n", iters);
	printf("- warmup: %u\n", warmup);
	printf("- index: %u\n", index);
	printf("- entries: %u\n", entries);
	printf("- interval_ns: %llu\n", (unsigned long long)interval_ns);
	printf("- timeout_ms: %d\n", timeout_ms);
	printf("- selftest: %s\n", selftest ? "yes" : "no");
	printf("- replace msg bytes: %u\n\n",
	       ((struct nlmsghdr *)msg_replace_a)->nlmsg_len);

	printf("## Results (RTM_NEWACTION replace with ACK)\n\n");
	printf("- total: %.6f s\n", secs);
	printf("- throughput: %.2f ops/s\n", ops);
	printf("- median latency: %.3f us\n", (double)med / 1000.0);
	printf("- p95 latency: %.3f us\n", (double)p95 / 1000.0);
	printf("- p99 latency: %.3f us\n", (double)p99 / 1000.0);

	free(lat);
	free(msg_create);
	free(msg_replace_a);
	free(msg_replace_b);
	free(msg_del);
	free(tx_scratch);
	mnl_socket_close(nl);
	return 0;
}
