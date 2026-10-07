#ifndef FIRM_NET_PACKET_H
#define FIRM_NET_PACKET_H

#include <stddef.h>
#include <stdint.h>

/*
 * Downstream ingest boundary.
 *
 * Every received item carries source identity and a first-arrival timestamp
 * so later feeds can race without rewriting OrbitFlare RX.
 *
 * data aliases preallocated ring-slot storage. It is valid only until the
 * corresponding ring acquire is released.
 */

#define NET_PKT_MAX 2048u
#define NET_RX_BATCH_MAX 64u

#define NET_SOURCE_INVALID    0u
#define NET_SOURCE_ORBITFLARE 1u
#define NET_SOURCE_DOUBLEZERO 2u /* reserved: feed #2, not implemented */
#define NET_SOURCE_BLOCKSPACE 3u /* reserved */
#define NET_SOURCE_MAX        8u

#define NET_PKT_TRUNC 0x01u

typedef struct {
    const uint8_t *data;
    uint16_t       len;
    uint8_t        source_id;
    uint8_t        flags;
    uint32_t       seq;
    uint64_t       rx_ns;
    uint64_t       rx_tsc;
} net_packet_t;

typedef struct {
    uint64_t rx_ns;
    uint64_t rx_tsc;
    uint32_t seq;
    uint16_t len;
    uint8_t  source_id;
    uint8_t  flags;
    uint8_t  data[NET_PKT_MAX];
} net_slot_t;

static inline int
net_source_valid(uint8_t source_id)
{
    return source_id > NET_SOURCE_INVALID && source_id < NET_SOURCE_MAX;
}

static inline int
net_packet_valid(const net_packet_t *p)
{
    if (p == NULL || p->data == NULL) {
        return 0;
    }
    if (p->len == 0u || p->len > NET_PKT_MAX) {
        return 0;
    }
    return net_source_valid(p->source_id);
}

static inline const char *
net_source_name(uint8_t source_id)
{
    switch (source_id) {
    case NET_SOURCE_ORBITFLARE:
        return "orbitflare";
    case NET_SOURCE_DOUBLEZERO:
        return "doublezero";
    case NET_SOURCE_BLOCKSPACE:
        return "blockspace";
    default:
        return "unknown";
    }
}

#endif /* FIRM_NET_PACKET_H */
