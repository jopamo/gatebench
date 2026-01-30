#ifndef GATEBENCH_NL_H
#define GATEBENCH_NL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Netlink socket handle (opaque structure) */
struct gb_nl_sock;

/* Netlink message buffer */
struct gb_nl_msg {
    void* buf;
    size_t cap;
    size_t len;
};

/* Initialize netlink socket */
int gb_nl_open(struct gb_nl_sock** sock);

/* Close netlink socket */
void gb_nl_close(struct gb_nl_sock* sock);

/* Send and receive netlink message with error checking */
int gb_nl_send_recv(struct gb_nl_sock* sock, struct gb_nl_msg* req, struct gb_nl_msg* resp, int timeout_ms);

/* Allocate netlink message buffer */
struct gb_nl_msg* gb_nl_msg_alloc(size_t capacity);

/* Free netlink message buffer */
void gb_nl_msg_free(struct gb_nl_msg* msg);

/* Reset message buffer (keep capacity, set len to 0) */
void gb_nl_msg_reset(struct gb_nl_msg* msg);

/* Forward declaration */
struct gate_dump;

/* Get gate action by index */
int gb_nl_get_action(struct gb_nl_sock* sock, uint32_t index, struct gate_dump* dump, int timeout_ms);

/* Get error string from netlink error */
const char* gb_nl_strerror(int err);

/* Check if error is expected (for selftests) */
bool gb_nl_error_expected(int err, int expected);

/* Get next sequence number */
uint32_t gb_nl_next_seq(struct gb_nl_sock* sock);

#endif /* GATEBENCH_NL_H */