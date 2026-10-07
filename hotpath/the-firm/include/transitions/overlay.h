#ifndef FIRM_TRANSITIONS_OVERLAY_H
#define FIRM_TRANSITIONS_OVERLAY_H

#include "ingress/tx.h"
#include "protocols/pump/pump.h"
#include "state/compact.h"

#include <stdint.h>

/*
 * Transaction-local overlay. Canonical CompactState is not written
 * here. Repeated visits read the provisional row.
 */

#define STATE_OVERLAY_MAX        8u
#define STATE_OVERLAY_TOKEN_MAX  16u
#define STATE_OVERLAY_SYS_MAX    8u

#define OV_OK       0
#define OV_FAIL     1
#define OV_UNKNOWN  2

typedef struct {
    uint32_t     pool_id;
    uint64_t     incarnation_base;
    pump_state_t pump;
    uint8_t      dirty;
} overlay_row_t;

typedef struct {
    uint32_t      id;
    uint64_t      incarnation_base;
    state_token_t tok;
    uint8_t       dirty;
} overlay_token_row_t;

typedef struct {
    uint32_t    id;
    uint64_t    incarnation_base;
    state_sys_t sys;
    uint8_t     dirty;
} overlay_sys_row_t;

typedef struct {
    overlay_row_t       row[STATE_OVERLAY_MAX];
    overlay_token_row_t token[STATE_OVERLAY_TOKEN_MAX];
    overlay_sys_row_t   sys[STATE_OVERLAY_SYS_MAX];
    uint8_t             n;
    uint8_t             n_token;
    uint8_t             n_sys;
    uint8_t             status;
} tx_overlay_t;

void overlay_clear(tx_overlay_t *ov);

int overlay_begin(tx_overlay_t *ov, const compact_state_t *st,
                  const ordered_tx_t *tx);

int overlay_ensure_token(tx_overlay_t *ov, const compact_state_t *st,
                         uint32_t id, uint8_t allow_dead,
                         overlay_token_row_t **out);

int overlay_ensure_sys(tx_overlay_t *ov, const compact_state_t *st,
                       uint32_t id, overlay_sys_row_t **out);

int overlay_apply_pump_ix(tx_overlay_t *ov, const compact_state_t *st,
                          const ordered_ix_t *ix, pump_swap_result_t *res);

int overlay_pump(const tx_overlay_t *ov, uint32_t pool_id,
                 const pump_state_t **out);

#endif /* FIRM_TRANSITIONS_OVERLAY_H */
