#ifndef FIRM_TX_PUMPSTATE_H
#define FIRM_TX_PUMPSTATE_H

#include "ingress/tx.h"
#include "state/compact.h"
#include "transitions/apply.h"
#include "tx/view.h"
#include "wire/classify.h"
#include "wire/lut_cache.h"

/*
 * PUMPSTATE-001. Framed bytes → overlay apply of supported PumpSwap
 * legs. Whole transaction, then publish once. Fail closed.
 */

#define PS_OK            0
#define PS_DECODE        1
#define PS_LUT           2
#define PS_UNSUPPORTED   3
#define PS_NO_PRESTATE   4
#define PS_APPLY         5
#define PS_IRRELEVANT    6
#define PS_GAP           7 /* pool present but not EXACT/READY */
#define PS_BEFORE_ANCHOR 8 /* slot already covered by snapshot */

typedef struct {
    int            rc;
    uint8_t        n_pump;
    uint8_t        lut_resolved;
    uint8_t        predicted;
    apply_result_t ar;
    ordered_tx_t   tx;
    tx_view_t      view;
} pumpstate_out_t;

const char *pumpstate_name(int rc);

int pumpstate_from_bytes(compact_state_t *st, const wire_lut_tab_t *luts,
                         const uint8_t *bytes, uint32_t len, uint64_t slot,
                         pumpstate_out_t *out);

int pumpstate_from_cache(compact_state_t *st, lut_cache_t *luts,
                         const uint8_t *bytes, uint32_t len, uint64_t slot,
                         pumpstate_out_t *out);

#endif
