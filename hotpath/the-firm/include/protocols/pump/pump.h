#ifndef FIRM_PROTOCOLS_PUMP_H
#define FIRM_PROTOCOLS_PUMP_H

#include <stdint.h>

/*
 * PumpSwap exact-in kernel. Bit-identical to arb-core CORE-006.
 * Program pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA
 *
 * Y_effective = quote_vault + virtual_quote, derived only here.
 * Exact-out buy inverts quote-in so reserve_base drops by base_out
 * (PUMP-TX-016 binary search over CORE-006 exact-in).
 */

#define PUMP_FEE_BPS_DEN       10000u
#define PUMP_DIR_QUOTE_TO_BASE 0u
#define PUMP_DIR_BASE_TO_QUOTE 1u
#define PUMP_DISABLE_BUY       8u
#define PUMP_DISABLE_SELL      16u

typedef struct {
    uint64_t reserve_base;
    uint64_t reserve_quote;
    int64_t  virtual_quote;
    uint64_t lp_fee_bps;
    uint64_t protocol_fee_bps;
    uint64_t creator_fee_bps;
    uint8_t  disabled;
    uint8_t  status;
} pump_state_t;

typedef struct {
    uint64_t amount_in;
    uint64_t min_amount_out;
    uint8_t  direction;
} pump_swap_ix_t;

typedef struct {
    uint64_t amount_in;
    uint64_t amount_out;
    uint64_t fee;
    uint64_t lp_fee;
    uint64_t protocol_fee;
    uint64_t creator_fee;
} pump_swap_result_t;

typedef struct {
    uint64_t amount_out;
    uint64_t fee;
    uint8_t  valid;
} pump_quote_t;

int pump_apply_swap(const pump_state_t *before,
                    const pump_swap_ix_t *ix,
                    pump_state_t *after,
                    pump_swap_result_t *result);

int pump_quote_exact_in(const pump_state_t *state,
                        uint64_t amount_in,
                        uint8_t direction,
                        pump_quote_t *out);

/* amount_in on result is the inverted quote spent. */
int pump_apply_buy_exact_out(const pump_state_t *before,
                             uint64_t base_out,
                             uint64_t max_quote,
                             pump_state_t *after,
                             pump_swap_result_t *result);

/*
 * Recover virtual_quote such that exact-in apply hits published vaults.
 * Explanation helper only — not a certified prestate source.
 */
int pump_invert_virtual(const pump_state_t *before, uint64_t amount_in,
                        uint8_t direction, uint64_t pub_base,
                        uint64_t pub_quote, int64_t *v_out);

#endif /* FIRM_PROTOCOLS_PUMP_H */
