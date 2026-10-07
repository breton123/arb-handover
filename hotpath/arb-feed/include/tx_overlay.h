#ifndef ARB_FEED_TX_OVERLAY_H
#define ARB_FEED_TX_OVERLAY_H

#include "dlmm.h"
#include "pump.h"
#include "swapix.h"

#include <stdint.h>

/*
 * TX-OVERLAY-001 — transaction-local replay.
 * Clone S_before, walk outer instructions in order, apply exact
 * DLMM/Pump swaps. Unknown pricing mutation on the watched pool
 * → fail closed (not TX_EXACT). Does not write canonical AUTH.
 *
 * IX_EXACT  decoded individual swap N
 * TX_EXACT  every pricing-relevant effect on the watched pool
 */

#define TXO_EXACT     1u
#define TXO_IX_ONLY   2u
#define TXO_UNKNOWN   3u
#define TXO_FAIL      4u

#define TXO_LEG_MAX   8u

typedef struct {
    uint8_t  proto;
    uint8_t  variant;
    uint8_t  exact;
    uint8_t  unknown_pricing;
    uint8_t  pool[32];
    uint64_t amount_in;
    uint64_t min_out;
    uint8_t  direction;
    uint8_t  have_dir;
    uint8_t  disc[8];
} txo_leg_t;

typedef struct {
    uint8_t     klass;      /* TXO_* */
    uint8_t     ix_exact;   /* decoded individual swap */
    uint8_t     tx_exact;   /* every pricing-relevant outer effect known */
    uint8_t     n_leg;
    uint8_t     n_exact;
    uint8_t     n_unknown;
    txo_leg_t   legs[TXO_LEG_MAX];
    dlmm_state_t s_dlmm;
    pump_state_t s_pump;
    uint8_t     have_dlmm;
    uint8_t     have_pump;
    uint64_t    last_fee;
    uint64_t    last_pfee;
    int32_t     last_active_after;
} tx_overlay_t;

int txo_scan(const uint8_t *p, uint32_t len, tx_overlay_t *out);

/* Restrict TX_EXACT to the watched pool. Other-pool unknown is not this S'. */
void txo_bind_watched(tx_overlay_t *ov, const uint8_t *pool);

/* Stamp N's decoded direction onto matching same-pool exact legs. */
void txo_stamp_n(tx_overlay_t *ov, const swapix_t *nix);

/* Apply exact same-pool legs onto S_before (already in *dlmm / *pump). */
int txo_apply(tx_overlay_t *ov, const uint8_t *pool, uint8_t proto,
              const dlmm_state_t *dlmm_before, const pump_state_t *pump_before);

#endif
