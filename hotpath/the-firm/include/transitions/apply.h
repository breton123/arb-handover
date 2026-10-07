#ifndef FIRM_TRANSITIONS_APPLY_H
#define FIRM_TRANSITIONS_APPLY_H

#include "deps/class.h"
#include "ingress/tx.h"
#include "state/cert.h"
#include "state/compact.h"
#include "transitions/overlay.h"

/*
 * apply_tx: classify → lookup → overlay → publish or rollback.
 * No RPC. Canonical mutation only after the whole transaction succeeds.
 */

#define APPLY_OK          0
#define APPLY_IRRELEVANT  1
#define APPLY_REJECT      2
#define APPLY_ABORTED     3

#define APPLY_STEP_NONE       0u
#define APPLY_STEP_CLASS      1u /* tx_class not FAST_CUSTOM after lookup */
#define APPLY_STEP_BEGIN      2u
#define APPLY_STEP_IX_CLASS   3u /* ix not FAST_CUSTOM in overlay loop */
#define APPLY_STEP_PUMP       4u
#define APPLY_STEP_PUMP_TOK   5u
#define APPLY_STEP_TOKEN      6u
#define APPLY_STEP_PUBLISH    7u

typedef struct {
    int          code;
    uint8_t      dep_class;
    uint8_t      have_off;
    uint8_t      n_no_quote_ix;
    uint8_t      fail_step;
    uint16_t     fail_ix;
    uint8_t      ix_class[STATE_IX_MAX];
    uint64_t     published_version;
    state_cert_t cert;
    tx_overlay_t overlay;
    dep_offend_t off;
} apply_result_t;

int apply_tx(compact_state_t *st, const ordered_tx_t *tx, apply_result_t *out);
const char *apply_step_name(uint8_t step);

#endif /* FIRM_TRANSITIONS_APPLY_H */
