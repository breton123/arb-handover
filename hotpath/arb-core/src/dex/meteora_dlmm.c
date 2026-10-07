#include "dlmm.h"

#include <string.h>

typedef unsigned __int128 u128;

#define ONE_Q64 ((u128)1 << DLMM_SCALE_OFFSET)

static int
u128_mul_shr64(u128 a, u128 b, int round_up, u128 *out)
{
    uint64_t a0 = (uint64_t)a;
    uint64_t a1 = (uint64_t)(a >> 64);
    uint64_t b0 = (uint64_t)b;
    uint64_t b1 = (uint64_t)(b >> 64);
    u128 p00 = (u128)a0 * b0;
    u128 p01 = (u128)a0 * b1;
    u128 p10 = (u128)a1 * b0;
    u128 p11 = (u128)a1 * b1;
    u128 mid = (p00 >> 64) + (uint64_t)p01 + (uint64_t)p10;
    u128 hi = p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
    u128 lo_hi = (uint64_t)mid;
    if (hi != 0) {
        return -1;
    }
    {
        u128 r = lo_hi;
    if (round_up) {
        uint64_t frac = (uint64_t)p00;
        if (frac != 0) {
            if (r == ~(u128)0) {
                return -1;
            }
            r++;
        }
    }
    *out = r;
    return 0;
    }
}

static int
u128_shl64_div(u128 num, u128 den, int round_up, u128 *out)
{
    u128 q;
    u128 rem;

    if (den == 0 || num > (~(u128)0 >> 64)) {
        return -1;
    }
    q = (num << 64) / den;
    rem = (num << 64) % den;
    if (round_up && rem != 0) {
        if (q == ~(u128)0) {
            return -1;
        }
        q++;
    }
    *out = q;
    return 0;
}

static int
amount_out(uint64_t in, u128 price, int swap_for_y, uint64_t *out)
{
    u128 r;
    if (swap_for_y) {
        if (u128_mul_shr64(price, (u128)in, 0, &r) != 0 || r > UINT64_MAX) {
            return -1;
        }
    } else if (u128_shl64_div((u128)in, price, 0, &r) != 0 || r > UINT64_MAX) {
        return -1;
    }
    *out = (uint64_t)r;
    return 0;
}

static int
amount_in(uint64_t out, u128 price, int swap_for_y, int round_up, uint64_t *in)
{
    u128 r;
    if (swap_for_y) {
        if (u128_shl64_div((u128)out, price, round_up, &r) != 0 || r > UINT64_MAX) {
            return -1;
        }
    } else if (u128_mul_shr64((u128)out, price, round_up, &r) != 0 || r > UINT64_MAX) {
        return -1;
    }
    *in = (uint64_t)r;
    return 0;
}

int
dlmm_price_from_id(int32_t id, uint16_t bin_step, u128 *out)
{
    u128 bps;
    u128 base;
    u128 squared;
    u128 result = ONE_Q64;
    uint32_t exp;
    int invert;

    if (id == 0) {
        *out = ONE_Q64;
        return 0;
    }
    invert = id < 0;
    exp = invert ? (uint32_t)(-id) : (uint32_t)id;
    if (exp >= 0x80000u) {
        return -1;
    }
    bps = ((u128)bin_step << DLMM_SCALE_OFFSET) / (u128)DLMM_BASIS_POINT_MAX;
    base = ONE_Q64 + bps;
    squared = base;
    if (squared >= result) {
        squared = ~(u128)0 / squared;
        invert = !invert;
    }
    {
        uint32_t bit;
        for (bit = 0; bit < 19; bit++) {
            if (exp & (1u << bit)) {
                u128 tmp;
                if (u128_mul_shr64(result, squared, 0, &tmp) != 0) {
                    return -1;
                }
                result = tmp;
            }
            if (bit + 1 < 19) {
                u128 tmp;
                if (u128_mul_shr64(squared, squared, 0, &tmp) != 0) {
                    return -1;
                }
                squared = tmp;
            }
        }
    }
    if (result == 0) {
        return -1;
    }
    if (invert) {
        result = ~(u128)0 / result;
    }
    *out = result;
    return 0;
}

static int
ensure_price(dlmm_bin_t *b, uint16_t bin_step)
{
    if (b->price != 0) {
        return 0;
    }
    return dlmm_price_from_id(b->id, bin_step, &b->price);
}

static int
total_fee_rate(const dlmm_state_t *s, u128 *out)
{
    u128 base;
    u128 variable = 0;
    u128 total;
    const dlmm_static_params_t *p = &s->parameters;
    uint32_t vol = s->v_parameters.volatility_accumulator;

    base = (u128)p->base_factor * (u128)s->bin_step * 10u;
    {
        uint8_t i;
        u128 pow10 = 1;
        for (i = 0; i < p->base_fee_power_factor; i++) {
            pow10 *= 10u;
        }
        base *= pow10;
    }
    if (p->variable_fee_control > 0) {
        u128 vfa = (u128)vol * (u128)s->bin_step;
        u128 sq = vfa * vfa;
        u128 v_fee = (u128)p->variable_fee_control * sq;
        variable = (v_fee + 99999999999ull) / 100000000000ull;
    }
    total = base + variable;
    if (total > DLMM_MAX_FEE_RATE) {
        total = DLMM_MAX_FEE_RATE;
    }
    *out = total;
    return 0;
}

static int
compute_fee(const dlmm_state_t *s, uint64_t amount, uint64_t *fee)
{
    u128 rate;
    u128 den;
    u128 f;

    if (total_fee_rate(s, &rate) != 0) {
        return -1;
    }
    den = (u128)DLMM_FEE_PRECISION - rate;
    if (den == 0) {
        return -1;
    }
    f = ((u128)amount * rate + den - 1) / den;
    if (f > UINT64_MAX) {
        return -1;
    }
    *fee = (uint64_t)f;
    return 0;
}

static int
compute_fee_from_amount(const dlmm_state_t *s, uint64_t amount_with_fees, uint64_t *fee)
{
    u128 rate;
    u128 f;

    if (total_fee_rate(s, &rate) != 0) {
        return -1;
    }
    f = ((u128)amount_with_fees * rate + (DLMM_FEE_PRECISION - 1)) / DLMM_FEE_PRECISION;
    if (f > UINT64_MAX) {
        return -1;
    }
    *fee = (uint64_t)f;
    return 0;
}

static int
compute_protocol_fee(const dlmm_state_t *s, uint64_t fee_amount, uint64_t *out)
{
    u128 p = (u128)fee_amount * s->parameters.protocol_share / DLMM_BASIS_POINT_MAX;
    if (p > UINT64_MAX) {
        return -1;
    }
    *out = (uint64_t)p;
    return 0;
}

static int
fee_on_input(const dlmm_state_t *s, int swap_for_y)
{
    if (s->parameters.collect_fee_mode == 1) {
        return !swap_for_y;
    }
    return 1;
}

static int
update_references(dlmm_state_t *s)
{
    int64_t elapsed = s->now_ts - s->v_parameters.last_update_timestamp;
    const dlmm_static_params_t *p = &s->parameters;

    if (elapsed >= (int64_t)p->filter_period) {
        s->v_parameters.index_reference = s->active_id;
        if (elapsed < (int64_t)p->decay_period) {
            uint64_t vr = (uint64_t)s->v_parameters.volatility_accumulator
                * p->reduction_factor / DLMM_BASIS_POINT_MAX;
            s->v_parameters.volatility_reference = (uint32_t)vr;
        } else {
            s->v_parameters.volatility_reference = 0;
        }
    }
    return 0;
}

static int
update_volatility(dlmm_state_t *s)
{
    int64_t delta = (int64_t)s->v_parameters.index_reference - (int64_t)s->active_id;
    if (delta < 0) {
        delta = -delta;
    }
    {
        uint64_t acc = (uint64_t)s->v_parameters.volatility_reference
            + (uint64_t)delta * DLMM_BASIS_POINT_MAX;
        if (acc > s->parameters.max_volatility_accumulator) {
            acc = s->parameters.max_volatility_accumulator;
        }
        s->v_parameters.volatility_accumulator = (uint32_t)acc;
    }
    return 0;
}

static dlmm_bin_t *
find_bin(dlmm_state_t *s, int32_t id)
{
    uint16_t i;
    for (i = 0; i < s->nbin; i++) {
        if (s->bins[i].id == id) {
            return &s->bins[i];
        }
    }
    return NULL;
}

static int
fill_mm(const dlmm_bin_t *bin, uint64_t amount, int swap_for_y,
        uint64_t *used_in, uint64_t *left, uint64_t *out_amt)
{
    uint64_t max_out = swap_for_y ? bin->amount_y : bin->amount_x;
    uint64_t max_in;

    if (max_out == 0) {
        *used_in = 0;
        *left = amount;
        *out_amt = 0;
        return 0;
    }
    if (amount_in(max_out, bin->price, swap_for_y, 1, &max_in) != 0) {
        return -1;
    }
    if (amount >= max_in) {
        *used_in = max_in;
        *left = amount - max_in;
        *out_amt = max_out;
        return 0;
    }
    if (amount_out(amount, bin->price, swap_for_y, out_amt) != 0) {
        return -1;
    }
    *used_in = amount;
    *left = 0;
    return 0;
}

int
dlmm_apply_swap(const dlmm_state_t *before,
                const dlmm_swap_ix_t *ix,
                dlmm_state_t *after,
                dlmm_apply_result_t *res)
{
    uint64_t left;
    uint64_t total_in = 0;
    uint64_t total_out = 0;
    uint64_t total_fee = 0;
    uint64_t total_proto = 0;
    int foi;
    uint32_t guard = 0;

    if (before == NULL || ix == NULL || after == NULL || res == NULL) {
        return -1;
    }
    if (before->status != 0 || ix->amount_in == 0) {
        return -1;
    }
    *after = *before;
    memset(res, 0, sizeof(*res));
    if (update_references(after) != 0) {
        return -1;
    }
    foi = fee_on_input(after, ix->swap_for_y);
    left = ix->amount_in;

    while (left > 0 && guard++ < 256) {
        dlmm_bin_t *bin = find_bin(after, after->active_id);
        uint64_t excluded;
        uint64_t used_in;
        uint64_t leftover;
        uint64_t out_amt;
        uint64_t fee = 0;
        uint64_t proto = 0;
        uint64_t included;
        uint64_t into_bin;

        if (bin == NULL) {
            return -1;
        }
        if (ensure_price(bin, after->bin_step) != 0) {
            return -1;
        }
        if (update_volatility(after) != 0) {
            return -1;
        }

        excluded = left;
        if (foi) {
            if (compute_fee_from_amount(after, left, &fee) != 0) {
                return -1;
            }
            if (fee > left) {
                return -1;
            }
            excluded = left - fee;
        }
        if (fill_mm(bin, excluded, ix->swap_for_y, &used_in, &leftover, &out_amt) != 0) {
            return -1;
        }
        included = left;
        if (leftover > 0) {
            excluded = excluded - leftover;
            if (foi) {
                if (compute_fee(after, excluded, &fee) != 0) {
                    return -1;
                }
                included = excluded + fee;
            } else {
                included = excluded;
            }
        }
        if (!foi) {
            if (compute_fee_from_amount(after, out_amt, &fee) != 0) {
                return -1;
            }
            if (fee > out_amt) {
                return -1;
            }
            out_amt -= fee;
        }
        if (compute_protocol_fee(after, fee, &proto) != 0) {
            return -1;
        }
        into_bin = included - fee;
        if (ix->swap_for_y) {
            bin->amount_x += into_bin;
            if (bin->amount_y < out_amt) {
                return -1;
            }
            bin->amount_y -= out_amt;
            after->reserve_x += into_bin;
            after->reserve_y -= out_amt;
        } else {
            bin->amount_y += into_bin;
            if (bin->amount_x < out_amt) {
                return -1;
            }
            bin->amount_x -= out_amt;
            after->reserve_y += into_bin;
            after->reserve_x -= out_amt;
        }
        if (included > left) {
            return -1;
        }
        left -= included;
        total_in += included;
        total_out += out_amt;
        total_fee += fee;
        total_proto += proto;
        if (left > 0) {
            if (ix->swap_for_y) {
                if (after->active_id <= DLMM_MIN_BIN_ID) {
                    return -1;
                }
                after->active_id--;
            } else {
                if (after->active_id >= DLMM_MAX_BIN_ID) {
                    return -1;
                }
                after->active_id++;
            }
        }
    }
    if (left > 0) {
        return -1;
    }
    if (total_out < ix->min_amount_out) {
        return -1;
    }
    res->amount_in = total_in;
    res->amount_out = total_out;
    res->fee = total_fee;
    res->protocol_fee = total_proto;
    res->active_id_after = after->active_id;
    return 0;
}

int
dlmm_quote_exact_in(const dlmm_state_t *state,
                    uint64_t amount_in,
                    uint8_t swap_for_y,
                    dlmm_quote_t *out)
{
    dlmm_state_t after;
    dlmm_swap_ix_t ix;
    dlmm_apply_result_t res;
    int32_t delta;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (state == NULL || amount_in == 0) {
        return -1;
    }
    ix.amount_in = amount_in;
    ix.min_amount_out = 0;
    ix.swap_for_y = swap_for_y ? 1 : 0;
    if (dlmm_apply_swap(state, &ix, &after, &res) != 0) {
        return -1;
    }
    delta = after.active_id - state->active_id;
    if (delta < 0) {
        delta = -delta;
    }
    out->amount_out = res.amount_out;
    out->fee = res.fee;
    out->bins_crossed = (uint16_t)delta;
    out->valid = 1;
    return 0;
}

int
dlmm_decode_swap2(const uint8_t *data, uint16_t len, dlmm_swap_ix_t *ix)
{
    uint64_t ain;
    uint64_t mino;

    if (data == NULL || ix == NULL || len < 8 + 8 + 8) {
        return -1;
    }
    if (memcmp(data, DLMM_SWAP2_DISC, 8) != 0) {
        return -1;
    }
    memcpy(&ain, data + 8, 8);
    memcpy(&mino, data + 16, 8);
    ix->amount_in = ain;
    ix->min_amount_out = mino;
    ix->swap_for_y = 0;
    return 0;
}
