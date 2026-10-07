#ifndef ARB_CORE_CYCLE_H
#define ARB_CORE_CYCLE_H

#include "dlmm.h"
#include "opportunity.h"
#include "pump.h"

#include <stdint.h>

/*
 * CORE-007 — fixed-size DLMM ↔ Pump cycle quote.
 *
 * Alignment (frozen, stupid): Pump quote = SOL, Pump base = TOKEN;
 * DLMM Y = SOL, DLMM X = TOKEN. Caller passes predicted S' for the
 * venue the trigger hit and canonical S for the other. This function
 * does not predict and does not write either state.
 *
 *   0  SOL → DLMM → TOKEN → Pump → SOL
 *   1  SOL → Pump → TOKEN → DLMM → SOL
 */

#define CYCLE_DLMM_THEN_PUMP  0u
#define CYCLE_PUMP_THEN_DLMM  1u

typedef struct {
    uint64_t amount_in;
    uint64_t amount_out;
    int64_t  gross_profit;
    uint8_t  direction;
    uint8_t  valid;
} cycle_quote_t;

int cycle_quote(const dlmm_state_t *dlmm,
                const pump_state_t *pump,
                uint64_t amount_in,
                uint8_t direction,
                cycle_quote_t *out);

int cycle_size(const dlmm_state_t *dlmm,
               const pump_state_t *pump,
               opportunity_t *out);

#endif /* ARB_CORE_CYCLE_H */
