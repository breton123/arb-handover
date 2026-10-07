#ifndef FIRM_NET_STATS_H
#define FIRM_NET_STATS_H

#include "packet.h"

#include <stdatomic.h>
#include <stdint.h>

/*
 * RX counters. Atomically incremented on the RX thread; snapshot from
 * a telemetry thread. No formatting on the hot path.
 *
 * first/late on this struct are unused. Keyed first/late live on
 * shred_race_stats_t.
 */

typedef struct {
    _Atomic uint64_t rx;
    _Atomic uint64_t bytes;
    _Atomic uint64_t malformed;
    _Atomic uint64_t ring_drops;
    _Atomic uint64_t cap_drops;
    _Atomic uint64_t trunc;
    _Atomic uint64_t rx_by_source[NET_SOURCE_MAX];
    _Atomic uint64_t bytes_by_source[NET_SOURCE_MAX];
    _Atomic uint64_t first_by_source[NET_SOURCE_MAX];
    _Atomic uint64_t late_by_source[NET_SOURCE_MAX];
} net_stats_t;

void net_stats_clear(net_stats_t *st);
void net_stats_snapshot(const net_stats_t *st, net_stats_t *out);

void net_stats_on_rx(net_stats_t *st, uint8_t source_id, uint16_t len);
void net_stats_on_malformed(net_stats_t *st);
void net_stats_on_ring_drop(net_stats_t *st, uint32_t n);
void net_stats_on_cap_drop(net_stats_t *st);
void net_stats_on_trunc(net_stats_t *st);
void net_stats_on_first(net_stats_t *st, uint8_t source_id);
void net_stats_on_late(net_stats_t *st, uint8_t source_id);

#endif /* FIRM_NET_STATS_H */
