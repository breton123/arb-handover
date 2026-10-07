#include "cycle.h"

#include <string.h>

int
cycle_quote(const dlmm_state_t *dlmm,
            const pump_state_t *pump,
            uint64_t amount_in,
            uint8_t direction,
            cycle_quote_t *out)
{
    dlmm_quote_t dq;
    pump_quote_t pq;
    uint64_t mid;
    uint64_t final;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->direction = direction;
    out->amount_in = amount_in;
    if (dlmm == NULL || pump == NULL || amount_in == 0
        || amount_in > (uint64_t)INT64_MAX) {
        return -1;
    }
    if (direction == CYCLE_DLMM_THEN_PUMP) {
        if (dlmm_quote_exact_in(dlmm, amount_in, 0, &dq) != 0 || !dq.valid) {
            return -1;
        }
        mid = dq.amount_out;
        if (pump_quote_exact_in(pump, mid, PUMP_DIR_BASE_TO_QUOTE, &pq) != 0
            || !pq.valid) {
            return -1;
        }
        final = pq.amount_out;
    } else if (direction == CYCLE_PUMP_THEN_DLMM) {
        if (pump_quote_exact_in(pump, amount_in, PUMP_DIR_QUOTE_TO_BASE, &pq) != 0
            || !pq.valid) {
            return -1;
        }
        mid = pq.amount_out;
        if (dlmm_quote_exact_in(dlmm, mid, 1, &dq) != 0 || !dq.valid) {
            return -1;
        }
        final = dq.amount_out;
    } else {
        return -1;
    }
    if (final > (uint64_t)INT64_MAX) {
        return -1;
    }
    out->amount_out = final;
    out->gross_profit = (int64_t)final - (int64_t)amount_in;
    out->valid = 1;
    return 0;
}

#define SOL_LAMPORTS     1000000000ull
#define SIZE_LADDER_N    13u
#define SIZE_REFINE_N    8u

static const uint64_t SIZE_LADDER[SIZE_LADDER_N] = {
    SOL_LAMPORTS / 100ull,  /* 0.01 */
    SOL_LAMPORTS / 50ull,   /* 0.02 */
    SOL_LAMPORTS / 20ull,   /* 0.05 */
    SOL_LAMPORTS / 10ull,   /* 0.1  */
    SOL_LAMPORTS / 5ull,    /* 0.2  */
    SOL_LAMPORTS / 2ull,    /* 0.5  */
    SOL_LAMPORTS,           /* 1    */
    SOL_LAMPORTS * 2ull,    /* 2    */
    SOL_LAMPORTS * 5ull,    /* 5    */
    SOL_LAMPORTS * 10ull,   /* 10   */
    SOL_LAMPORTS * 20ull,   /* 20   */
    SOL_LAMPORTS * 50ull,   /* 50   */
    SOL_LAMPORTS * 100ull   /* 100  */
};

static void
consider(opportunity_t *best, const cycle_quote_t *q)
{
    if (q == NULL || !q->valid || q->gross_profit <= 0) {
        return;
    }
    if (best->valid
        && ((uint64_t)q->gross_profit < best->gross_profit
            || ((uint64_t)q->gross_profit == best->gross_profit
                && q->amount_in >= best->amount_in))) {
        return;
    }
    best->route_id = ROUTE_DLMM_PUMP;
    best->amount_in = q->amount_in;
    best->amount_out = q->amount_out;
    best->gross_profit = (uint64_t)q->gross_profit;
    best->direction = q->direction;
    best->valid = 1;
}

static int
eval_size(const dlmm_state_t *dlmm, const pump_state_t *pump,
          uint64_t ain, uint8_t dir, opportunity_t *best)
{
    cycle_quote_t q;

    if (cycle_quote(dlmm, pump, ain, dir, &q) != 0) {
        return 0;
    }
    consider(best, &q);
    return 0;
}

int
cycle_size(const dlmm_state_t *dlmm,
           const pump_state_t *pump,
           opportunity_t *out)
{
    opportunity_t best;
    uint32_t i;
    uint32_t k;
    uint8_t dir;
    uint64_t lo;
    uint64_t hi;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (dlmm == NULL || pump == NULL) {
        return -1;
    }
    memset(&best, 0, sizeof(best));
    for (dir = 0; dir < 2; dir++) {
        for (i = 0; i < SIZE_LADDER_N; i++) {
            (void)eval_size(dlmm, pump, SIZE_LADDER[i], dir, &best);
        }
    }
    if (!best.valid) {
        return 0;
    }
    lo = best.amount_in / 2ull;
    hi = best.amount_in * 2ull;
    for (i = 0; i < SIZE_LADDER_N; i++) {
        if (SIZE_LADDER[i] == best.amount_in) {
            if (i > 0) {
                lo = SIZE_LADDER[i - 1];
            }
            if (i + 1 < SIZE_LADDER_N) {
                hi = SIZE_LADDER[i + 1];
            }
            break;
        }
    }
    if (lo < 1) {
        lo = 1;
    }
    if (hi <= lo) {
        *out = best;
        return 0;
    }
    for (k = 1; k <= SIZE_REFINE_N; k++) {
        uint64_t ain = lo + (hi - lo) * (uint64_t)k / (uint64_t)(SIZE_REFINE_N + 1u);
        if (ain == 0 || ain == best.amount_in) {
            continue;
        }
        (void)eval_size(dlmm, pump, ain, best.direction, &best);
    }
    *out = best;
    return 0;
}
