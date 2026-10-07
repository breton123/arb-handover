#ifndef ARB_EXEC_LEADER_H
#define ARB_EXEC_LEADER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/*
 * EXEC-005 — sender stub.
 *
 * Connected UDP. Control plane opens the socket. Hot path is send().
 * DoubleZero replaces this. Do not grow it into a TPU client.
 */

#define LEADER_N  8u

typedef struct {
    int fd;
} leader_conn_t;

int leader_conn_open(leader_conn_t *conn, const char *ip, uint16_t port);
void leader_conn_close(leader_conn_t *conn);

static inline int
leader_send(leader_conn_t *conn, const uint8_t *tx, uint16_t len)
{
    ssize_t n;

    if (conn == NULL || tx == NULL || conn->fd < 0 || len == 0) {
        return -1;
    }
    n = send(conn->fd, tx, (size_t)len, 0);
    if (n != (ssize_t)len) {
        return -1;
    }
    return 0;
}

#endif /* ARB_EXEC_LEADER_H */
