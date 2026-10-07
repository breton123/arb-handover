#ifndef FIRM_NET_UDP_H
#define FIRM_NET_UDP_H

#include <stdint.h>

/*
 * IPv4 UDP bind used by every feed. Feed identity is not here.
 * Linux production path. Not compiled on other hosts.
 */

#if defined(__linux__)

typedef struct {
    int fd;
} net_udp_t;

int net_udp_open(net_udp_t *s, const char *ip, uint16_t port);
int net_udp_set_rcvbuf(net_udp_t *s, int bytes);
int net_udp_set_busy_poll(net_udp_t *s, int busy_us, int budget);
void net_udp_close(net_udp_t *s);

#endif /* __linux__ */

#endif /* FIRM_NET_UDP_H */
