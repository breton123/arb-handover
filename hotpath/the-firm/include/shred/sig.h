#ifndef FIRM_SHRED_SIG_H
#define FIRM_SHRED_SIG_H

#include <stdint.h>

/*
 * Fee-payer Ed25519 check. Cold path. Not a leader-schedule check.
 * 1 = ok, 0 = fail, -1 = unavailable / unparsable.
 */
int shred_tx_sig_verify(const uint8_t *tx, uint32_t len);

#endif
