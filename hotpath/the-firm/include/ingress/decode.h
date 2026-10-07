#ifndef FIRM_INGRESS_DECODE_H
#define FIRM_INGRESS_DECODE_H

#include "ingress/tx.h"

#include <stdint.h>

/*
 * Reconstruction output: framed Solana tx bytes → OrderedTx.
 * ALT lookups are not resolved here (FALLBACK).
 */

#define DEC_OK           0
#define DEC_INCOMPLETE   1
#define DEC_INVALID      2
#define DEC_ALT          3

int ingress_decode_tx(const uint8_t *bytes, uint32_t len, uint64_t slot,
                      ordered_tx_t *out);

#endif /* FIRM_INGRESS_DECODE_H */
