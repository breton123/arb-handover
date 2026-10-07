#include "feed.h"

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int
udp_source_open(udp_source_t *s, const char *ip, uint16_t port)
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
    {
        struct timeval tv;
        memset(&tv, 0, sizeof(tv));
        tv.tv_usec = 200000;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    s->fd = fd;
    return 0;
}

int
udp_source_set_rcvbuf(udp_source_t *s, int bytes)
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
udp_source_recv(udp_source_t *s, uint8_t *buf, uint16_t cap, rx_packet_t *out)
{
    ssize_t n;

    if (s == NULL || buf == NULL || out == NULL || s->fd < 0 || cap == 0) {
        return -1;
    }
    n = recv(s->fd, buf, cap, 0);
    if (n <= 0) {
        return -1;
    }
    if (n > 65535) {
        return -1;
    }
    out->data  = buf;
    out->len   = (uint16_t)n;
    out->queue = 0;
    return 0;
}

void
udp_source_close(udp_source_t *s)
{
    if (s == NULL || s->fd < 0) {
        return;
    }
    close(s->fd);
    s->fd = -1;
}
