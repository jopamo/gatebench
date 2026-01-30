#include "../include/gatebench_nl.h"
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

/* Netlink socket structure */
struct gb_nl_sock {
    struct mnl_socket *nl;
    uint32_t pid;
    uint32_t seq;
};

int gb_nl_open(struct gb_nl_sock **sock) {
    struct gb_nl_sock *s;
    struct mnl_socket *nl;
    
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

void gb_nl_close(struct gb_nl_sock *sock) {
    if (sock && sock->nl) {
        mnl_socket_close(sock->nl);
        sock->nl = NULL;
    }
}

struct gb_nl_msg *gb_nl_msg_alloc(size_t capacity) {
    struct gb_nl_msg *msg;
    
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

void gb_nl_msg_free(struct gb_nl_msg *msg) {
    if (msg) {
        if (msg->buf) {
            free(msg->buf);
        }
        free(msg);
    }
}

void gb_nl_msg_reset(struct gb_nl_msg *msg) {
    if (msg) {
        msg->len = 0;
    }
}

static int parse_error(const struct nlmsghdr *nlh) {
    const struct nlmsgerr *err;
    
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
        return -EPROTO;
    }
    
    err = mnl_nlmsg_get_payload(nlh);
    
    /* Netlink errors are negative errno values */
    return err->error;
}

static int recv_response(struct gb_nl_sock *sock, 
                        struct gb_nl_msg *resp,
                        uint32_t expected_seq,
                        int timeout_ms) {
    struct pollfd pfd;
    ssize_t ret;
    int len;
    struct nlmsghdr *nlh;
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
        nlh = (struct nlmsghdr *)resp->buf;
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

int gb_nl_send_recv(struct gb_nl_sock *sock,
                    struct gb_nl_msg *req,
                    struct gb_nl_msg *resp,
                    int timeout_ms) {
    ssize_t ret;
    struct nlmsghdr *nlh;
    uint32_t seq;
    
    if (!sock || !sock->nl || !req || !resp) {
        return -EINVAL;
    }
    
    if (req->len > req->cap) {
        return -EINVAL;
    }
    
    /* Get and set sequence number */
    seq = gb_nl_next_seq(sock);
    nlh = (struct nlmsghdr *)req->buf;
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

const char *gb_nl_strerror(int err) {
    /* Netlink errors are negative errno values */
    if (err >= 0) {
        return "Success";
    }
    
    return strerror(-err);
}

bool gb_nl_error_expected(int err, int expected) {
    if (expected >= 0) {
        return err == expected;
    }
    
    return err < 0 && err == expected;
}

/* Helper to get next sequence number */
uint32_t gb_nl_next_seq(struct gb_nl_sock *sock) {
    if (!sock) {
        return 0;
    }
    
    return sock->seq++;
}
