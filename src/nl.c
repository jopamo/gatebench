#include "../include/gatebench_nl.h"
#include "../include/gatebench_gate.h"
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_cls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

/* Netlink socket structure */
struct gb_nl_sock {
    struct mnl_socket* nl;
    uint32_t pid;
    uint32_t seq;
};

int gb_nl_open(struct gb_nl_sock** sock) {
    struct gb_nl_sock* s;
    struct mnl_socket* nl;

    if (!sock) {
        return -EINVAL;
    }

    s = malloc(sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }

    memset(s, 0, sizeof(*s));

    /* Open netlink socket for routing (NETLINK_ROUTE) */
    nl = mnl_socket_open(NETLINK_ROUTE);
    if (!nl) {
        free(s);
        return -errno;
    }

    /* Bind with automatic PID assignment */
    if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(nl);
        free(s);
        return -errno;
    }

    s->nl = nl;
    s->pid = mnl_socket_get_portid(nl);
    s->seq = 1;

    *sock = s;
    return 0;
}

void gb_nl_close(struct gb_nl_sock* sock) {
    if (!sock) {
        return;
    }

    if (sock->nl) {
        mnl_socket_close(sock->nl);
        sock->nl = NULL;
    }

    free(sock);
}

struct gb_nl_msg* gb_nl_msg_alloc(size_t capacity) {
    struct gb_nl_msg* msg;

    msg = malloc(sizeof(*msg));
    if (!msg) {
        return NULL;
    }

    msg->buf = malloc(capacity);
    if (!msg->buf) {
        free(msg);
        return NULL;
    }

    msg->cap = capacity;
    msg->len = 0;

    return msg;
}

void gb_nl_msg_free(struct gb_nl_msg* msg) {
    if (msg) {
        if (msg->buf) {
            free(msg->buf);
        }
        free(msg);
    }
}

void gb_nl_msg_reset(struct gb_nl_msg* msg) {
    if (msg) {
        msg->len = 0;
    }
}

static int parse_error(const struct nlmsghdr* nlh) {
    const struct nlmsgerr* err;

    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
        return -EPROTO;
    }

    err = mnl_nlmsg_get_payload(nlh);

    /* Netlink errors are negative errno values */
    return err->error;
}

static int nl_attr_cb_copy(const struct nlattr* attr, void* data) {
    const struct nlattr** tb = data;
    unsigned int type = mnl_attr_get_type(attr);

    if (type <= TCA_ROOT_MAX)
        tb[type] = attr;
    return MNL_CB_OK;
}

static int parse_delaction_fcnt(const struct nlmsghdr* nlh, uint32_t* fcnt_out) {
    const struct nlattr* tb[TCA_ROOT_MAX + 1] = {NULL};
    struct nlattr* attr;
    struct nlattr* inner;

    if (!fcnt_out)
        return -EINVAL;

    if (mnl_attr_parse(nlh, sizeof(struct tcamsg), nl_attr_cb_copy, tb) < 0)
        return -EINVAL;

    if (!tb[TCA_ACT_TAB])
        return -ENOENT;

    mnl_attr_for_each_nested(attr, tb[TCA_ACT_TAB]) {
        if (mnl_attr_get_type(attr) != 0)
            continue;

        mnl_attr_for_each_nested(inner, attr) {
            if (mnl_attr_get_type(inner) != TCA_FCNT)
                continue;
            if (mnl_attr_get_payload_len(inner) < sizeof(uint32_t))
                return -EINVAL;
            *fcnt_out = mnl_attr_get_u32(inner);
            return 0;
        }
    }

    return -ENOENT;
}

static int recv_response(struct gb_nl_sock* sock, struct gb_nl_msg* resp, uint32_t expected_seq, int timeout_ms) {
    struct pollfd pfd;
    ssize_t ret;
    int len;
    struct nlmsghdr* nlh;
    int done = 0;

    if (!sock || !sock->nl || !resp) {
        return -EINVAL;
    }

    pfd.fd = mnl_socket_get_fd(sock->nl);
    pfd.events = POLLIN;

    while (!done) {
        /* Wait for data with timeout */
        ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            return -errno;
        }
        if (ret == 0) {
            return -ETIMEDOUT;
        }

        /* Receive data */
        ret = mnl_socket_recvfrom(sock->nl, resp->buf, resp->cap);
        if (ret < 0) {
            return -errno;
        }

        resp->len = (size_t)ret;
        len = (int)ret;

        /* Parse netlink message */
        nlh = (struct nlmsghdr*)resp->buf;
        while (mnl_nlmsg_ok(nlh, len)) {
            /* Check if this is our response */
            if (nlh->nlmsg_seq == expected_seq) {
                if (nlh->nlmsg_type == NLMSG_ERROR) {
                    return parse_error(nlh);
                }
                if (nlh->nlmsg_type == NLMSG_DONE) {
                    return 0;
                }
                /* For other message types, assume success */
                return 0;
            }

            nlh = mnl_nlmsg_next(nlh, &len);
        }
    }

    return -ENOMSG; /* No matching sequence found */
}

static int recv_ack(struct gb_nl_sock* sock, struct gb_nl_msg* resp, uint32_t expected_seq, int timeout_ms) {
    struct pollfd pfd;
    ssize_t ret;
    int len;
    struct nlmsghdr* nlh;

    if (!sock || !sock->nl || !resp) {
        return -EINVAL;
    }

    pfd.fd = mnl_socket_get_fd(sock->nl);
    pfd.events = POLLIN;

    for (;;) {
        ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            return -errno;
        }
        if (ret == 0) {
            return -ETIMEDOUT;
        }

        ret = mnl_socket_recvfrom(sock->nl, resp->buf, resp->cap);
        if (ret < 0) {
            return -errno;
        }

        resp->len = (size_t)ret;
        len = (int)ret;
        nlh = (struct nlmsghdr*)resp->buf;
        while (mnl_nlmsg_ok(nlh, len)) {
            if (nlh->nlmsg_seq == expected_seq) {
                if (nlh->nlmsg_type == NLMSG_ERROR) {
                    return parse_error(nlh);
                }
            }
            nlh = mnl_nlmsg_next(nlh, &len);
        }
    }
}

int gb_nl_send_recv(struct gb_nl_sock* sock, struct gb_nl_msg* req, struct gb_nl_msg* resp, int timeout_ms) {
    ssize_t ret;
    struct nlmsghdr* nlh;
    uint32_t seq;

    if (!sock || !sock->nl || !req || !resp) {
        return -EINVAL;
    }

    if (req->len > req->cap) {
        return -EINVAL;
    }

    /* Get and set sequence number */
    seq = gb_nl_next_seq(sock);
    nlh = (struct nlmsghdr*)req->buf;
    nlh->nlmsg_seq = seq;

    /* Send request */
    ret = mnl_socket_sendto(sock->nl, req->buf, req->len);
    if (ret < 0) {
        return -errno;
    }

    if ((size_t)ret != req->len) {
        return -EIO;
    }

    /* Receive response */
    return recv_response(sock, resp, seq, timeout_ms);
}

int gb_nl_send_recv_ack(struct gb_nl_sock* sock, struct gb_nl_msg* req, struct gb_nl_msg* resp, int timeout_ms) {
    ssize_t ret;
    struct nlmsghdr* nlh;
    uint32_t seq;

    if (!sock || !sock->nl || !req || !resp) {
        return -EINVAL;
    }

    if (req->len > req->cap) {
        return -EINVAL;
    }

    seq = gb_nl_next_seq(sock);
    nlh = (struct nlmsghdr*)req->buf;
    nlh->nlmsg_seq = seq;

    ret = mnl_socket_sendto(sock->nl, req->buf, req->len);
    if (ret < 0) {
        return -errno;
    }

    if ((size_t)ret != req->len) {
        return -EIO;
    }

    return recv_ack(sock, resp, seq, timeout_ms);
}

int gb_nl_send_recv_flush(struct gb_nl_sock* sock,
                          struct gb_nl_msg* req,
                          struct gb_nl_msg* resp,
                          int timeout_ms,
                          uint32_t* fcnt_out) {
    struct pollfd pfd;
    ssize_t ret;
    int len;
    struct nlmsghdr* nlh;
    uint32_t seq;

    if (!sock || !sock->nl || !req || !resp)
        return -EINVAL;

    if (req->len > req->cap)
        return -EINVAL;

    if (fcnt_out)
        *fcnt_out = UINT32_MAX;

    seq = gb_nl_next_seq(sock);
    nlh = (struct nlmsghdr*)req->buf;
    nlh->nlmsg_seq = seq;

    ret = mnl_socket_sendto(sock->nl, req->buf, req->len);
    if (ret < 0)
        return -errno;

    if ((size_t)ret != req->len)
        return -EIO;

    pfd.fd = mnl_socket_get_fd(sock->nl);
    pfd.events = POLLIN;

    for (;;) {
        ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0)
            return -errno;
        if (ret == 0)
            return -ETIMEDOUT;

        ret = mnl_socket_recvfrom(sock->nl, resp->buf, resp->cap);
        if (ret < 0)
            return -errno;

        resp->len = (size_t)ret;
        len = (int)ret;
        nlh = (struct nlmsghdr*)resp->buf;
        while (mnl_nlmsg_ok(nlh, len)) {
            if (nlh->nlmsg_seq != seq) {
                nlh = mnl_nlmsg_next(nlh, &len);
                continue;
            }

            if (nlh->nlmsg_type == RTM_DELACTION && fcnt_out) {
                uint32_t fcnt = UINT32_MAX;
                if (parse_delaction_fcnt(nlh, &fcnt) == 0)
                    *fcnt_out = fcnt;
            }

            if (nlh->nlmsg_type == NLMSG_ERROR)
                return parse_error(nlh);

            nlh = mnl_nlmsg_next(nlh, &len);
        }
    }
}

int gb_nl_get_action(struct gb_nl_sock* sock, uint32_t index, struct gate_dump* dump, int timeout_ms) {
    struct gb_nl_msg* req = NULL;
    struct gb_nl_msg* resp = NULL;
    int ret;

    req = gb_nl_msg_alloc(1024);
    resp = gb_nl_msg_alloc((size_t)MNL_SOCKET_BUFFER_SIZE);

    if (!req || !resp) {
        ret = -ENOMEM;
        goto out;
    }

    ret = build_gate_getaction(req, index);
    if (ret < 0) {
        goto out;
    }

    ret = gb_nl_send_recv(sock, req, resp, timeout_ms);
    if (ret < 0) {
        goto out;
    }

    ret = gb_nl_gate_parse((struct nlmsghdr*)resp->buf, dump);

out:
    if (req)
        gb_nl_msg_free(req);
    if (resp)
        gb_nl_msg_free(resp);
    return ret;
}

int gb_nl_dump_action(struct gb_nl_sock* sock, struct gb_nl_msg* req, struct gb_dump_stats* stats, int timeout_ms) {
    struct gb_nl_msg* resp = NULL;
    struct nlmsghdr* nlh;
    struct pollfd pfd;
    uint32_t seq;
    ssize_t ret;
    int len;

    if (!sock || !sock->nl || !req || !stats)
        return -EINVAL;

    memset(stats, 0, sizeof(*stats));

    if (req->len > req->cap)
        return -EINVAL;

    resp = gb_nl_msg_alloc(1024u * 1024u);
    if (!resp)
        return -ENOMEM;

    seq = gb_nl_next_seq(sock);
    nlh = (struct nlmsghdr*)req->buf;
    nlh->nlmsg_seq = seq;

    ret = mnl_socket_sendto(sock->nl, req->buf, req->len);
    if (ret < 0) {
        gb_nl_msg_free(resp);
        return -errno;
    }

    if ((size_t)ret != req->len) {
        gb_nl_msg_free(resp);
        return -EIO;
    }

    pfd.fd = mnl_socket_get_fd(sock->nl);
    pfd.events = POLLIN;

    for (;;) {
        ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            gb_nl_msg_free(resp);
            return -errno;
        }
        if (ret == 0) {
            gb_nl_msg_free(resp);
            return -ETIMEDOUT;
        }

        ret = mnl_socket_recvfrom(sock->nl, resp->buf, resp->cap);
        if (ret < 0) {
            gb_nl_msg_free(resp);
            return -errno;
        }

        resp->len = (size_t)ret;
        len = (int)ret;
        nlh = (struct nlmsghdr*)resp->buf;

        while (mnl_nlmsg_ok(nlh, len)) {
            if (nlh->nlmsg_seq != seq) {
                nlh = mnl_nlmsg_next(nlh, &len);
                continue;
            }

            if (nlh->nlmsg_type == NLMSG_ERROR) {
                int err = parse_error(nlh);
                if (err != 0) {
                    stats->saw_error = true;
                    stats->error_code = err;
                    gb_nl_msg_free(resp);
                    return 0;
                }
                nlh = mnl_nlmsg_next(nlh, &len);
                continue;
            }

            if (nlh->nlmsg_type == NLMSG_DONE) {
                stats->saw_done = true;
                gb_nl_msg_free(resp);
                return 0;
            }

            if (nlh->nlmsg_type == RTM_GETACTION) {
                const struct nlattr* tb[TCA_ROOT_MAX + 1] = {NULL};

                if (mnl_attr_parse(nlh, sizeof(struct tcamsg), nl_attr_cb_copy, tb) == 0 && tb[TCA_ROOT_COUNT]) {
                    stats->action_count += mnl_attr_get_u32(tb[TCA_ROOT_COUNT]);
                }
            }

            stats->reply_msgs++;
            if (nlh->nlmsg_len >= NLMSG_HDRLEN)
                stats->payload_bytes += (uint64_t)(nlh->nlmsg_len - NLMSG_HDRLEN);

            nlh = mnl_nlmsg_next(nlh, &len);
        }
    }
}

const char* gb_nl_strerror(int err) {
    /* Netlink errors are negative errno values */
    if (err >= 0) {
        return "Success";
    }

    return strerror(-err);
}

bool gb_nl_error_expected(int err, int expected) {
    if (expected == GB_NL_EXPECT_COMPAT) {
        return err == 0 || err == -EINVAL;
    }

    if (expected >= 0) {
        return err == expected;
    }

    return err < 0 && err == expected;
}

/* Helper to get next sequence number */
uint32_t gb_nl_next_seq(struct gb_nl_sock* sock) {
    if (!sock) {
        return 0;
    }

    return sock->seq++;
}
