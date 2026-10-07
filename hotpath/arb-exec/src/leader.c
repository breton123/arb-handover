#include "leader.h"

#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

int
leader_conn_open(leader_conn_t *conn, const char *ip, uint16_t port)
{
    struct sockaddr_in dst;
    int fd;

    if (conn == NULL || ip == NULL || port == 0) {
        return -1;
    }
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        close(fd);
        return -1;
    }
    conn->fd = fd;
    return 0;
}

void
leader_conn_close(leader_conn_t *conn)
{
    if (conn == NULL || conn->fd < 0) {
        return;
    }
    close(conn->fd);
    conn->fd = -1;
}
