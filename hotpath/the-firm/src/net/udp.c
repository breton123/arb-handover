#if !defined(__linux__)
#error "src/net/udp.c is Linux-only"
#endif

#include "net/udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif

int
net_udp_open(net_udp_t *s, const char *ip, uint16_t port)
{
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    if (s == NULL || ip == NULL || port == 0) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    s->fd = fd;
    return 0;
}

int
net_udp_set_rcvbuf(net_udp_t *s, int bytes)
{
    int got;
    socklen_t n;

    if (s == NULL || s->fd < 0 || bytes < 65536) {
        return -1;
    }
    if (setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    n = sizeof(got);
    if (getsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &got, &n) != 0) {
        return -1;
    }
    /* Linux reports 2x the requested value. */
    if (got < bytes) {
        return -1;
    }
    return got;
}

int
net_udp_set_busy_poll(net_udp_t *s, int busy_us, int budget)
{
    if (s == NULL || s->fd < 0 || busy_us < 1) {
        return -1;
    }
    if (setsockopt(s->fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us,
                   sizeof(busy_us)) != 0) {
        return -1;
    }
#ifdef SO_BUSY_POLL_BUDGET
    if (budget > 0) {
        (void)setsockopt(s->fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget,
                         sizeof(budget));
    }
#else
    (void)budget;
#endif
    return 0;
}

void
net_udp_close(net_udp_t *s)
{
    if (s == NULL || s->fd < 0) {
        return;
    }
    close(s->fd);
    s->fd = -1;
}
