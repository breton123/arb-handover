#ifndef ARB_CORE_RX_H
#define ARB_CORE_RX_H

#include "shred.h"

#include <stdint.h>

/*
 * arb-nic contract. The searcher only sees this.
 *
 *   NETWORK → arb-nic → rx_packet_t → payload pointer + length
 *
 * Replay fills the same struct. DoubleZero later replaces the source.
 */
typedef struct {
    const uint8_t *data;
    uint16_t len;
    uint16_t queue;
} rx_packet_t;

static inline int
hot_rx(rx_packet_t *pkt, shred_view_t *view)
{
    return shred_parse(pkt->data, pkt->len, view);
}

#endif /* ARB_CORE_RX_H */
