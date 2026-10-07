#ifndef FIRM_TRANSITIONS_TOKEN_H
#define FIRM_TRANSITIONS_TOKEN_H

#include "ingress/tx.h"
#include "protocols/pump/pump.h"
#include "state/compact.h"
#include "transitions/overlay.h"

int overlay_apply_pump_tokens(tx_overlay_t *ov, const compact_state_t *st,
                              const ordered_ix_t *ix,
                              const pump_swap_result_t *res,
                              const pump_state_t *after);

int overlay_apply_token_ix(tx_overlay_t *ov, const compact_state_t *st,
                           const ordered_ix_t *ix);

#endif /* FIRM_TRANSITIONS_TOKEN_H */
