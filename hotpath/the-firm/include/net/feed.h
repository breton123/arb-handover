#ifndef FIRM_NET_FEED_H
#define FIRM_NET_FEED_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "packet.h"
#include "ring.h"
#include "stats.h"

#if defined(__linux__)
#include "udp.h"
#endif

#include <stdint.h>

/*
 * One independent RX feed.
 *
 * Adding DoubleZero / Blockspace is another net_feed_t with a new
 * source_id and socket. OrbitFlare code does not change.
 *
 * Hot path: claim ingress slots → recvmmsg into payloads → stamp
 * source/timestamps → optional lossy capture copy → commit.
 *
 * net_feed_t is large (discard batch). Allocate once at startup.
 */

#define NET_ORBITFLARE_DEFAULT_PORT 20001u

#if defined(__linux__)

#include <sys/socket.h>

typedef struct {
    uint8_t      source_id;
    uint32_t     batch;
    uint32_t     seq;
    net_udp_t    udp;
    net_ring_t  *ingress;
    net_ring_t  *capture; /* nullable */
    net_stats_t *stats;   /* nullable */
    net_slot_t  *claimed[NET_RX_BATCH_MAX];
    struct mmsghdr msgs[NET_RX_BATCH_MAX];
    struct iovec   iov[NET_RX_BATCH_MAX];
    uint8_t        discard[NET_RX_BATCH_MAX][NET_PKT_MAX];
} net_feed_t;

int net_feed_open(net_feed_t *f, uint8_t source_id,
                  const char *ip, uint16_t port,
                  net_ring_t *ingress, net_ring_t *capture,
                  net_stats_t *stats, uint32_t batch);

int net_orbitflare_open(net_feed_t *f, const char *ip, uint16_t port,
                        net_ring_t *ingress, net_ring_t *capture,
                        net_stats_t *stats, uint32_t batch);

int net_feed_set_rcvbuf(net_feed_t *f, int bytes);
int net_feed_set_busy_poll(net_feed_t *f, int busy_us);

/*
 * One receive burst.
 * wait_ms >= 0: poll(2) that long, then recvmmsg
 * wait_ms <  0: one non-blocking recvmmsg (busy mode)
 * Returns packets committed to the ingress ring, 0 on timeout/EAGAIN,
 * or -1 on fatal socket error.
 */
int net_feed_poll(net_feed_t *f, int wait_ms);

void net_feed_close(net_feed_t *f);

#endif /* __linux__ */

#endif /* FIRM_NET_FEED_H */
