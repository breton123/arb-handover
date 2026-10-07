#include "dlmm_orders.h"

#include <string.h>

/*
 * Offline only. Not in lib `core`.
 * Fee walk matches CORE-003. Layer debit is the new contract.
 * fulfilled_order_* / limit_order_fee_* are not mutated here — program
 * apply source is not in the public SDK quote path. Inventory layers are.
 */

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

static int
ensure_price(dlmm_o_bin_t *b, uint16_t bin_step)
{
    (void)bin_step;
    /* Offline tests set price. Landing generation may call dlmm_price_from_id. */
    if (b->price != 0) {
        return 0;
    }
    return -1;
}

int
dlmm_o_orders_enabled(const dlmm_o_state_t *s)
{
    if (s == NULL) {
        return 0;
    }
    if (s->function_type == DLMM_FN_LIMIT_ORDER) {
        return 1;
    }
    if (s->function_type == DLMM_FN_LIQUIDITY_MINING) {
        return 0;
    }
    if (s->function_type == DLMM_FN_UNDETERMINED) {
        return s->reward_mint_live_mask == 0;
    }
    return 0;
}

int
dlmm_o_side_matches(const dlmm_o_bin_t *bin, int swap_for_y)
{
    int ask = bin->limit_order_ask_side != 0;
    return (swap_for_y && !ask) || (!swap_for_y && ask);
}

void
dlmm_o_available(const dlmm_o_bin_t *bin, int swap_for_y, int orders_on,
                 uint64_t *mm, uint64_t *proc, uint64_t *open_amt)
{
    *mm = swap_for_y ? bin->amount_y : bin->amount_x;
    *proc = 0;
    *open_amt = 0;
    if (orders_on && dlmm_o_side_matches(bin, swap_for_y)) {
        *proc = bin->processed_order_remaining_amount;
        *open_amt = bin->open_order_amount;
    }
}

static int
fill_against(uint64_t max_out, u128 price, uint64_t amount, int swap_for_y,
             uint64_t *used_in, uint64_t *left, uint64_t *out_amt)
{
    uint64_t max_in;

    if (max_out == 0) {
        *used_in = 0;
        *left = amount;
        *out_amt = 0;
        return 0;
    }
    if (amount_in(max_out, price, swap_for_y, 1, &max_in) != 0) {
        return -1;
    }
    if (amount >= max_in) {
        *used_in = max_in;
        *left = amount - max_in;
        *out_amt = max_out;
        return 0;
    }
    if (amount_out(amount, price, swap_for_y, out_amt) != 0) {
        return -1;
    }
    *used_in = amount;
    *left = 0;
    return 0;
}

int
dlmm_o_fill_mm(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
               uint64_t *used_in, uint64_t *left, uint64_t *out_amt)
{
    uint64_t max_out = swap_for_y ? bin->amount_y : bin->amount_x;
    return fill_against(max_out, bin->price, amount, swap_for_y, used_in, left, out_amt);
}

int
dlmm_o_fill_processed_order(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
                            int orders_on, uint64_t *used_in, uint64_t *left,
                            uint64_t *out_amt)
{
    uint64_t max_out = 0;
    if (orders_on && dlmm_o_side_matches(bin, swap_for_y)) {
        max_out = bin->processed_order_remaining_amount;
    }
    return fill_against(max_out, bin->price, amount, swap_for_y, used_in, left, out_amt);
}

int
dlmm_o_fill_open_orders(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
                        int orders_on, uint64_t *used_in, uint64_t *left,
                        uint64_t *out_amt)
{
    uint64_t max_out = 0;
    if (orders_on && dlmm_o_side_matches(bin, swap_for_y)) {
        max_out = bin->open_order_amount;
    }
    return fill_against(max_out, bin->price, amount, swap_for_y, used_in, left, out_amt);
}

int
dlmm_o_fill_bin(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
                int orders_on, dlmm_o_fill_t *out)
{
    uint64_t left;

    if (bin == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (dlmm_o_fill_mm(bin, amount, swap_for_y, &out->mm_in, &left, &out->mm_out) != 0) {
        return -1;
    }
    if (dlmm_o_fill_processed_order(bin, left, swap_for_y, orders_on,
                                    &out->proc_in, &left, &out->proc_out) != 0) {
        return -1;
    }
    if (dlmm_o_fill_open_orders(bin, left, swap_for_y, orders_on,
                               &out->open_in, &left, &out->open_out) != 0) {
        return -1;
    }
    out->left = left;
    out->used_in = out->mm_in + out->proc_in + out->open_in;
    out->out_amt = out->mm_out + out->proc_out + out->open_out;
    return 0;
}

int
dlmm_o_apply_fill(dlmm_o_bin_t *bin, dlmm_o_state_t *s, int swap_for_y,
                  const dlmm_o_fill_t *f)
{
    if (bin == NULL || s == NULL || f == NULL) {
        return -1;
    }
    if (swap_for_y) {
        if (bin->amount_y < f->mm_out) {
            return -1;
        }
        bin->amount_y -= f->mm_out;
        bin->amount_x += f->mm_in;
        s->reserve_y -= f->out_amt;
        s->reserve_x += f->used_in;
    } else {
        if (bin->amount_x < f->mm_out) {
            return -1;
        }
        bin->amount_x -= f->mm_out;
        bin->amount_y += f->mm_in;
        s->reserve_x -= f->out_amt;
        s->reserve_y += f->used_in;
    }
    if (bin->processed_order_remaining_amount < f->proc_out
        || bin->open_order_amount < f->open_out) {
        return -1;
    }
    bin->processed_order_remaining_amount -= f->proc_out;
    bin->open_order_amount -= f->open_out;
    return 0;
}

static int
total_fee_rate(const dlmm_o_state_t *s, u128 *out)
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
compute_fee(const dlmm_o_state_t *s, uint64_t amount, uint64_t *fee)
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
compute_fee_from_amount(const dlmm_o_state_t *s, uint64_t amount_with_fees, uint64_t *fee)
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
compute_protocol_fee(const dlmm_o_state_t *s, uint64_t fee_amount, uint64_t *out)
{
    u128 p = (u128)fee_amount * s->parameters.protocol_share / DLMM_BASIS_POINT_MAX;
    if (p > UINT64_MAX) {
        return -1;
    }
    *out = (uint64_t)p;
    return 0;
}

static int
fee_on_input(const dlmm_o_state_t *s, int swap_for_y)
{
    if (s->parameters.collect_fee_mode == 1) {
        return !swap_for_y;
    }
    return 1;
}

static int
update_references(dlmm_o_state_t *s)
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
update_volatility(dlmm_o_state_t *s)
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

static dlmm_o_bin_t *
find_bin(dlmm_o_state_t *s, int32_t id)
{
    uint16_t i;
    for (i = 0; i < s->nbin; i++) {
        if (s->bins[i].id == id) {
            return &s->bins[i];
        }
    }
    return NULL;
}

int
dlmm_o_apply_swap(const dlmm_o_state_t *before,
                  const dlmm_swap_ix_t *ix,
                  dlmm_o_state_t *after,
                  dlmm_apply_result_t *res)
{
    uint64_t left;
    uint64_t total_in = 0;
    uint64_t total_out = 0;
    uint64_t total_fee = 0;
    uint64_t total_proto = 0;
    int foi;
    int orders_on;
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
    orders_on = dlmm_o_orders_enabled(after);
    left = ix->amount_in;

    while (left > 0 && guard++ < 256) {
        dlmm_o_bin_t *bin = find_bin(after, after->active_id);
        dlmm_o_fill_t fill;
        uint64_t excluded;
        uint64_t leftover;
        uint64_t out_amt;
        uint64_t fee = 0;
        uint64_t proto = 0;
        uint64_t included;
        uint64_t used_in;

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
        if (dlmm_o_fill_bin(bin, excluded, ix->swap_for_y, orders_on, &fill) != 0) {
            return -1;
        }
        leftover = fill.left;
        out_amt = fill.out_amt;
        used_in = fill.used_in;
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
        if (dlmm_o_apply_fill(bin, after, ix->swap_for_y, &fill) != 0) {
            return -1;
        }
        if (included > left) {
            return -1;
        }
        left -= included;
        total_in += included;
        total_out += out_amt;
        total_fee += fee;
        total_proto += proto;
        (void)used_in;
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
dlmm_o_quote_exact_in(const dlmm_o_state_t *state,
                      uint64_t amount_in,
                      uint8_t swap_for_y,
                      dlmm_quote_t *out)
{
    dlmm_o_state_t after;
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
    if (dlmm_o_apply_swap(state, &ix, &after, &res) != 0) {
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
