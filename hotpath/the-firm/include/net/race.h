#ifndef FIRM_NET_RACE_H
#define FIRM_NET_RACE_H

#include "packet.h"
#include "ring.h"
#include "stats.h"

#include <stdint.h>

/*
 * Ingress mux, not the keyed race.
 *
 * Each feed owns a ring. This pops the next ready packet immediately
 * (round-robin among rings that have data). rx_ns is evidence, not a
 * sort key. Do not wait for other feeds.
 *
 * Keyed first-writer-wins lives in shred/race.h.
 */

typedef struct {
    net_ring_t  *ring[NET_SOURCE_MAX];
    uint8_t      n;
    uint8_t      next; /* RR start */
    uint8_t      held; /* 1-based ring index of outstanding acquire */
    net_stats_t *stats;
} net_race_t;

void net_race_init(net_race_t *r, net_stats_t *stats);
int net_race_add(net_race_t *r, net_ring_t *ring);

int net_race_acquire(net_race_t *r, net_packet_t *out);
void net_race_release(net_race_t *r);

#endif /* FIRM_NET_RACE_H */
