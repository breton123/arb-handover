#ifndef ARB_CORE_ADAPTER_H
#define ARB_CORE_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Same contract as frozen DLMM/Pump:
 *   apply_swap(S, N, S')
 *   quote_exact_in(S, amount, direction)
 * direction 0 = X→Y, 1 = Y→X.
 */

#define ADAPTER_OK        0
#define ADAPTER_FAIL      1
#define ADAPTER_NO_LIQ    2

#define AMM2_FEE_DENOM    10000u

typedef struct {
    uint64_t reserve_x;
    uint64_t reserve_y;
    uint16_t fee_bps;
    uint8_t  status;
} amm2_state_t;

typedef struct {
    int32_t  tick;
    uint64_t sqrt_price_x64;
    uint64_t liquidity;
    uint16_t fee_bps;
    uint8_t  status;
    uint8_t  has_ticks;
} clmm_state_t;

int amm2_quote(const amm2_state_t *s, uint64_t amount_in, int dir,
               uint64_t *amount_out);
int amm2_apply(const amm2_state_t *s, uint64_t amount_in, int dir,
               amm2_state_t *out, uint64_t *amount_out);

int clmm_quote(const clmm_state_t *s, uint64_t amount_in, int dir,
               uint64_t *amount_out);
int clmm_apply(const clmm_state_t *s, uint64_t amount_in, int dir,
               clmm_state_t *out, uint64_t *amount_out);

#endif /* ARB_CORE_ADAPTER_H */
