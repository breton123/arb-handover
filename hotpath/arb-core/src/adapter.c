#include "adapter.h"

#include <string.h>

static uint64_t
mul_div(uint64_t a, uint64_t b, uint64_t d)
{
    __uint128_t n;

    if (d == 0) {
        return 0;
    }
    n = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)(n / (__uint128_t)d);
}

int
amm2_quote(const amm2_state_t *s, uint64_t amount_in, int dir,
           uint64_t *amount_out)
{
    uint64_t rin;
    uint64_t rout;
    uint64_t fee_adj;
    uint64_t out;

    if (s == NULL || amount_out == NULL || amount_in == 0 || s->status == 0
        || s->reserve_x == 0 || s->reserve_y == 0
        || s->fee_bps >= AMM2_FEE_DENOM) {
        return ADAPTER_FAIL;
    }
    if (dir == 0) {
        rin = s->reserve_x;
        rout = s->reserve_y;
    } else if (dir == 1) {
        rin = s->reserve_y;
        rout = s->reserve_x;
    } else {
        return ADAPTER_FAIL;
    }
    fee_adj = mul_div(amount_in, (uint64_t)(AMM2_FEE_DENOM - s->fee_bps),
                      AMM2_FEE_DENOM);
    if (fee_adj == 0) {
        return ADAPTER_NO_LIQ;
    }
    out = mul_div(rout, fee_adj, rin + fee_adj);
    if (out == 0 || out >= rout) {
        return ADAPTER_NO_LIQ;
    }
    *amount_out = out;
    return ADAPTER_OK;
}

int
amm2_apply(const amm2_state_t *s, uint64_t amount_in, int dir,
           amm2_state_t *out, uint64_t *amount_out)
{
    uint64_t ao;

    if (out == NULL) {
        return ADAPTER_FAIL;
    }
    if (amm2_quote(s, amount_in, dir, &ao) != ADAPTER_OK) {
        return ADAPTER_FAIL;
    }
    *out = *s;
    if (dir == 0) {
        out->reserve_x += amount_in;
        out->reserve_y -= ao;
    } else {
        out->reserve_y += amount_in;
        out->reserve_x -= ao;
    }
    if (amount_out != NULL) {
        *amount_out = ao;
    }
    return ADAPTER_OK;
}

static int
clmm_as_amm2(const clmm_state_t *s, amm2_state_t *a)
{
    uint64_t sqrtp;
    uint64_t l;

    if (s == NULL || a == NULL || s->status == 0 || !s->has_ticks
        || s->liquidity == 0 || s->sqrt_price_x64 == 0) {
        return ADAPTER_NO_LIQ;
    }
    sqrtp = s->sqrt_price_x64;
    l = s->liquidity;
    memset(a, 0, sizeof(*a));
    a->reserve_x = (uint64_t)(((__uint128_t)l << 64) / sqrtp);
    a->reserve_y = (uint64_t)(((__uint128_t)l * sqrtp) >> 64);
    a->fee_bps = s->fee_bps;
    a->status = 1;
    if (a->reserve_x == 0 || a->reserve_y == 0) {
        return ADAPTER_NO_LIQ;
    }
    return ADAPTER_OK;
}

int
clmm_quote(const clmm_state_t *s, uint64_t amount_in, int dir,
           uint64_t *amount_out)
{
    amm2_state_t a;
    uint64_t out;
    int rc;

    rc = clmm_as_amm2(s, &a);
    if (rc != ADAPTER_OK) {
        return rc;
    }
    rc = amm2_quote(&a, amount_in, dir, &out);
    if (rc != ADAPTER_OK) {
        return rc;
    }
    /*
     * Single-tick / fail-closed: reject if the trade would exhaust more
     * than half the in-range virtual reserve (would cross a missing tick).
     */
    if (dir == 0 && out * 2u >= a.reserve_y) {
        return ADAPTER_NO_LIQ;
    }
    if (dir == 1 && out * 2u >= a.reserve_x) {
        return ADAPTER_NO_LIQ;
    }
    *amount_out = out;
    return ADAPTER_OK;
}

int
clmm_apply(const clmm_state_t *s, uint64_t amount_in, int dir,
           clmm_state_t *out, uint64_t *amount_out)
{
    uint64_t ao;
    amm2_state_t a;
    amm2_state_t ap;

    if (out == NULL) {
        return ADAPTER_FAIL;
    }
    if (clmm_quote(s, amount_in, dir, &ao) != ADAPTER_OK) {
        return ADAPTER_FAIL;
    }
    if (clmm_as_amm2(s, &a) != ADAPTER_OK) {
        return ADAPTER_FAIL;
    }
    if (amm2_apply(&a, amount_in, dir, &ap, NULL) != ADAPTER_OK) {
        return ADAPTER_FAIL;
    }
    *out = *s;
    if (amount_out != NULL) {
        *amount_out = ao;
    }
    (void)ap;
    return ADAPTER_OK;
}
