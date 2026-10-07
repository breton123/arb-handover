#include "pump.h"

#include <string.h>

typedef unsigned __int128 u128;

static int
ceil_div(u128 n, u128 d, u128 *out)
{
    if (d == 0) {
        return -1;
    }
    *out = n / d + (u128)(n % d != 0);
    return 0;
}

static int
fee_bps(u128 n, uint64_t bps, u128 *out)
{
    if (bps > PUMP_FEE_BPS_DEN) {
        return -1;
    }
    return ceil_div(n * (u128)bps, (u128)PUMP_FEE_BPS_DEN, out);
}

static int
effective_quote(const pump_state_t *s, u128 *out)
{
    __int128 q;

    if (s->reserve_base == 0 || s->reserve_quote == 0) {
        return -1;
    }
    q = (__int128)s->reserve_quote + (__int128)s->virtual_quote;
    if (q <= 0 || q > (__int128)UINT64_MAX) {
        return -1;
    }
    *out = (u128)q;
    return 0;
}

static int
split_fees(u128 n, const pump_state_t *s, u128 *lp, u128 *proto, u128 *creator)
{
    if (fee_bps(n, s->lp_fee_bps, lp) != 0
        || fee_bps(n, s->protocol_fee_bps, proto) != 0
        || fee_bps(n, s->creator_fee_bps, creator) != 0) {
        return -1;
    }
    return 0;
}

int
pump_apply_swap(const pump_state_t *before,
                const pump_swap_ix_t *ix,
                pump_state_t *after,
                pump_swap_result_t *result)
{
    u128 qeff;
    u128 total_bps;
    u128 out = 0;
    u128 fee = 0;
    u128 lp = 0;
    u128 proto = 0;
    u128 creator = 0;
    uint8_t disable_bit;

    if (before == NULL || ix == NULL || after == NULL || result == NULL) {
        return -1;
    }
    if (before->status != 0 || ix->amount_in == 0) {
        return -1;
    }
    if (ix->direction != PUMP_DIR_QUOTE_TO_BASE
        && ix->direction != PUMP_DIR_BASE_TO_QUOTE) {
        return -1;
    }
    disable_bit = (ix->direction == PUMP_DIR_QUOTE_TO_BASE)
        ? PUMP_DISABLE_BUY
        : PUMP_DISABLE_SELL;
    if (before->disabled & disable_bit) {
        return -1;
    }
    total_bps = (u128)before->lp_fee_bps + before->protocol_fee_bps
        + before->creator_fee_bps;
    if (total_bps >= PUMP_FEE_BPS_DEN) {
        return -1;
    }
    if (effective_quote(before, &qeff) != 0) {
        return -1;
    }

    if (ix->direction == PUMP_DIR_BASE_TO_QUOTE) {
        u128 gross;
        u128 leave;

        gross = qeff * (u128)ix->amount_in
            / ((u128)before->reserve_base + (u128)ix->amount_in);
        if (split_fees(gross, before, &lp, &proto, &creator) != 0) {
            return -1;
        }
        fee = lp + proto + creator;
        if (gross < fee) {
            return -1;
        }
        leave = gross - lp;
        if (leave > (u128)before->reserve_quote) {
            return -1;
        }
        out = gross - fee;
        if ((u128)before->reserve_base + (u128)ix->amount_in > UINT64_MAX) {
            return -1;
        }
        *after = *before;
        after->reserve_base = before->reserve_base + ix->amount_in;
        after->reserve_quote = before->reserve_quote - (uint64_t)leave;
    } else {
        u128 effective;
        u128 net;

        effective = (u128)ix->amount_in * (u128)PUMP_FEE_BPS_DEN
            / ((u128)PUMP_FEE_BPS_DEN + total_bps);
        if (split_fees(effective, before, &lp, &proto, &creator) != 0) {
            return -1;
        }
        fee = lp + proto + creator;
        if (effective + fee > (u128)ix->amount_in) {
            effective -= effective + fee - (u128)ix->amount_in;
            if (split_fees(effective, before, &lp, &proto, &creator) != 0) {
                return -1;
            }
            fee = lp + proto + creator;
        }
        if (effective < 1) {
            return -1;
        }
        net = effective - 1;
        out = (u128)before->reserve_base * net / (qeff + net);
        if (out == 0 || out > (u128)before->reserve_base) {
            return -1;
        }
        if (proto + creator > (u128)ix->amount_in) {
            return -1;
        }
        if ((u128)before->reserve_quote + (u128)ix->amount_in > UINT64_MAX) {
            return -1;
        }
        *after = *before;
        after->reserve_base = before->reserve_base - (uint64_t)out;
        after->reserve_quote = before->reserve_quote
            + ix->amount_in - (uint64_t)proto - (uint64_t)creator;
    }

    if (out == 0 || out > UINT64_MAX || fee > UINT64_MAX) {
        return -1;
    }
    if ((uint64_t)out < ix->min_amount_out) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->amount_in = ix->amount_in;
    result->amount_out = (uint64_t)out;
    result->fee = (uint64_t)fee;
    result->lp_fee = (uint64_t)lp;
    result->protocol_fee = (uint64_t)proto;
    result->creator_fee = (uint64_t)creator;
    return 0;
}

int
pump_quote_exact_in(const pump_state_t *state,
                    uint64_t amount_in,
                    uint8_t direction,
                    pump_quote_t *out)
{
    pump_state_t after;
    pump_swap_ix_t ix;
    pump_swap_result_t res;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (state == NULL || amount_in == 0) {
        return -1;
    }
    ix.amount_in = amount_in;
    ix.min_amount_out = 0;
    ix.direction = direction;
    if (pump_apply_swap(state, &ix, &after, &res) != 0) {
        return -1;
    }
    out->amount_out = res.amount_out;
    out->fee = res.fee;
    out->valid = 1;
    return 0;
}
