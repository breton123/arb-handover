#ifndef ARB_CORE_RX_H
#define ARB_CORE_RX_H

#include "shred.h"

#include <stdint.h>

/*
 * Exact arb-core / arb-nic contract. Do not diverge.
 *
 *   NETWORK → arb-feed → rx_packet_t → arb-core
 *
 * Replay fills the same struct. Timestamps live on the capture
 * record, not here.
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
