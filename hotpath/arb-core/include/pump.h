#ifndef ARB_CORE_PUMP_H
#define ARB_CORE_PUMP_H

#include <stdint.h>

/*
 * CORE-006 — PumpSwap exact-in kernel.
 * Program pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA
 *
 * Math from the vendored PumpSwap SDK (buyQuoteInput / sellBaseInput)
 * and the official 10k integer vectors. Constant product on
 * (base, quote + virtual_quote). Fees are ceil(n * bps / 10000).
 *
 * Restricted: exact-in, classic SPL, no Token-2022, no mayhem/cashback,
 * no fee-tier lookup (rates are already resolved into state).
 * Unsupported → fail closed.
 */

#define PUMP_FEE_BPS_DEN       10000u

#define PUMP_DIR_QUOTE_TO_BASE 0u /* buy:  amount_in is quote */
#define PUMP_DIR_BASE_TO_QUOTE 1u /* sell: amount_in is base  */

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
    uint8_t  status; /* 0 enabled; nonzero (mayhem/cashback) → fail */
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

#endif /* ARB_CORE_PUMP_H */
