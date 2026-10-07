#ifndef FIRM_SHRED_COMPLETE_H
#define FIRM_SHRED_COMPLETE_H

#include "shred/shred.h"

#include <stdint.h>

/*
 * First complete transaction from a DATA_COMPLETE shred payload.
 * Not FEC. Spanning entries return SHRED_TX_WAIT.
 */

#define SHRED_TX_OK     0
#define SHRED_TX_WAIT   1
#define SHRED_TX_BAD    2

#define SHRED_OFF_FLAGS 85u
#define SHRED_OFF_SIZE  86u
#define SHRED_DATA_COMPLETE 0x80u

int shred_first_tx(const uint8_t *pkt, uint16_t len, uint64_t *slot,
                   const uint8_t **tx, uint32_t *tx_len);

#endif /* FIRM_SHRED_COMPLETE_H */
