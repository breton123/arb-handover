#ifndef FIRM_NET_SINK_H
#define FIRM_NET_SINK_H

#include "packet.h"
#include "stats.h"

#include <stdatomic.h>
#include <stdint.h>

/*
 * Temporary validation sink. Checks the ingest contract and counts.
 * No shred parse, no protocol decode.
 *
 * Counters are atomic so a telemetry thread may snapshot them.
 */

typedef struct {
    _Atomic uint64_t ok;
    _Atomic uint64_t bad;
    _Atomic uint64_t bytes;
    _Atomic uint64_t by_source[NET_SOURCE_MAX];
} net_sink_t;

void net_sink_clear(net_sink_t *s);
int net_sink_consume(net_sink_t *s, const net_packet_t *pkt);

#endif /* FIRM_NET_SINK_H */
