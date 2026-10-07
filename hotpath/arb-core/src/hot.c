#include "hot.h"
#include "cycle.h"

#include <string.h>

#define SOL_LAMPORTS   1000000000ull
#define SIZE_LADDER_N  13u
#define SIZE_REFINE_N  8u

static const uint64_t SIZE_LADDER[SIZE_LADDER_N] = {
    SOL_LAMPORTS / 100ull,
    SOL_LAMPORTS / 50ull,
    SOL_LAMPORTS / 20ull,
    SOL_LAMPORTS / 10ull,
    SOL_LAMPORTS / 5ull,
    SOL_LAMPORTS / 2ull,
    SOL_LAMPORTS,
    SOL_LAMPORTS * 2ull,
    SOL_LAMPORTS * 5ull,
    SOL_LAMPORTS * 10ull,
    SOL_LAMPORTS * 20ull,
    SOL_LAMPORTS * 50ull,
    SOL_LAMPORTS * 100ull
};

static int
quote_hop(const universe_t *u, const univ_state_t *st,
          uint32_t pool_idx, int dir, uint64_t ain, uint64_t *aout)
{
    const pool_meta_t *p;
    uint32_t si;

    if (u == NULL || st == NULL || pool_idx >= u->n_pool || aout == NULL) {
        return -1;
    }
    p = &u->pool[pool_idx];
    si = p->state_idx;
    if (si >= st->n) {
        return -1;
    }
    switch (p->protocol) {
    case PROTO_CLMM:
    case PROTO_ORCA:
        return clmm_quote(&st->clmm[si], ain, dir, aout) == ADAPTER_OK ? 0 : -1;
    case PROTO_CPMM:
    case PROTO_DAMM:
        return amm2_quote(&st->amm2[si], ain, dir, aout) == ADAPTER_OK ? 0 : -1;
    case PROTO_DLMM:
    case PROTO_PUMP:
        return -1; /* HOT-001: frozen kernels only, never amm2 */
    default:
        return -1;
    }
}

int
hot_quote_route(const universe_t *u, const univ_state_t *st,
                uint32_t route_idx, uint64_t amount_in,
                uint64_t *amount_out)
{
    const compiled_route_t *r;
    uint64_t cur;
    uint8_t h;

    if (u == NULL || st == NULL || amount_out == NULL || route_idx >= u->n_route) {
        return -1;
    }
    r = &u->route[route_idx];
    if (r->family == ROUTE_FAM_0_DLMM_PUMP) {
        return -1;
    }
    if (r->n_hop < 2 || r->n_hop > UNIV_HOP_MAX || amount_in == 0) {
        return -1;
    }
    cur = amount_in;
    for (h = 0; h < r->n_hop; h++) {
        uint64_t nxt;
        if (quote_hop(u, st, r->pool[h], r->dir[h], cur, &nxt) != 0) {
            return -1;
        }
        cur = nxt;
    }
    *amount_out = cur;
    return 0;
}

static void
consider(opportunity_t *best, uint32_t route_id, uint64_t ain, uint64_t aout)
{
    uint64_t gp;

    if (aout <= ain) {
        return;
    }
    gp = aout - ain;
    if (best->valid
        && (gp < best->gross_profit
            || (gp == best->gross_profit && ain >= best->amount_in))) {
        return;
    }
    best->route_id = route_id;
    best->amount_in = ain;
    best->amount_out = aout;
    best->gross_profit = gp;
    best->direction = 0;
    best->valid = 1;
}

int
hot_apply_pool(const universe_t *u, univ_state_t *st,
               uint32_t pool_idx, uint64_t amount_in, int dir)
{
    const pool_meta_t *p;
    uint32_t si;
    uint64_t ao;

    if (u == NULL || st == NULL || pool_idx >= u->n_pool) {
        return -1;
    }
    p = &u->pool[pool_idx];
    si = p->state_idx;
    if (si >= st->n) {
        return -1;
    }
    switch (p->protocol) {
    case PROTO_CLMM:
    case PROTO_ORCA:
        return clmm_apply(&st->clmm[si], amount_in, dir, &st->clmm[si], &ao)
            == ADAPTER_OK ? 0 : -1;
    case PROTO_CPMM:
    case PROTO_DAMM:
        return amm2_apply(&st->amm2[si], amount_in, dir, &st->amm2[si], &ao)
            == ADAPTER_OK ? 0 : -1;
    default:
        return -1;
    }
}

int
hot_eval_pool(const universe_t *u, const univ_state_t *st,
              uint32_t pool_idx, opportunity_t *best)
{
    const pool_meta_t *p;
    uint32_t i;
    uint32_t k;

    if (best == NULL) {
        return -1;
    }
    memset(best, 0, sizeof(*best));
    if (u == NULL || st == NULL || pool_idx >= u->n_pool) {
        return -1;
    }
    p = &u->pool[pool_idx];
    for (i = 0; i < p->route_n; i++) {
        uint32_t rid = u->rindex[p->route_off + i];
        if (rid >= u->n_route || u->route[rid].family == ROUTE_FAM_0_DLMM_PUMP) {
            continue;
        }
        uint64_t aout;
        uint32_t s;
        uint64_t best_in = 0;
        uint64_t best_out = 0;
        int have = 0;

        for (s = 0; s < SIZE_LADDER_N; s++) {
            if (hot_quote_route(u, st, rid, SIZE_LADDER[s], &aout) != 0) {
                continue;
            }
            if (aout > SIZE_LADDER[s]
                && (!have || (aout - SIZE_LADDER[s]) > (best_out - best_in)
                    || ((aout - SIZE_LADDER[s]) == (best_out - best_in)
                        && SIZE_LADDER[s] < best_in))) {
                best_in = SIZE_LADDER[s];
                best_out = aout;
                have = 1;
            }
        }
        if (!have) {
            continue;
        }
        {
            uint64_t lo = best_in / 2ull;
            uint64_t hi = best_in * 2ull;
            for (s = 0; s < SIZE_LADDER_N; s++) {
                if (SIZE_LADDER[s] == best_in) {
                    if (s > 0) {
                        lo = SIZE_LADDER[s - 1];
                    }
                    if (s + 1 < SIZE_LADDER_N) {
                        hi = SIZE_LADDER[s + 1];
                    }
                    break;
                }
            }
            if (lo < 1) {
                lo = 1;
            }
            if (hi > lo) {
                for (k = 1; k <= SIZE_REFINE_N; k++) {
                    uint64_t ain = lo + (hi - lo) * (uint64_t)k
                        / (uint64_t)(SIZE_REFINE_N + 1u);
                    if (ain == 0 || ain == best_in) {
                        continue;
                    }
                    if (hot_quote_route(u, st, rid, ain, &aout) != 0) {
                        continue;
                    }
                    if (aout > ain
                        && (aout - ain) > (best_out - best_in)) {
                        best_in = ain;
                        best_out = aout;
                    }
                }
            }
        }
        consider(best, rid, best_in, best_out);
    }
    return 0;
}

static int
quote_live_hop(const live_univ_t *u, uint32_t pool_idx, int dir, uint64_t ain,
               uint64_t *aout, uint32_t pred_idx,
               const dlmm_pool_hot_t *pred_dlmm, const pump_state_t *pred_pump)
{
    uint8_t proto;

    if (u == NULL || aout == NULL || pool_idx >= u->n || ain == 0) {
        return -1;
    }
    proto = u->meta[pool_idx].protocol;
    if (proto == PROTO_DLMM) {
        dlmm_state_t s;
        dlmm_quote_t q;
        const dlmm_pool_hot_t *h;
        uint8_t swap_for_y = (dir == 0) ? 1u : 0u;
        if (pred_dlmm != NULL && pool_idx == pred_idx) {
            dlmm_hot_to_state(pred_dlmm, &s);
        } else {
            h = dlmm_cache_get(&u->dlmm, pool_idx);
            if (h == NULL) {
                return -1;
            }
            dlmm_hot_to_state(h, &s);
        }
        if (dlmm_quote_exact_in(&s, ain, swap_for_y, &q) != 0 || !q.valid) {
            return -1;
        }
        *aout = q.amount_out;
        return 0;
    }
    if (proto == PROTO_PUMP) {
        const pump_state_t *ps;
        pump_quote_t q;
        uint8_t pdir = (dir == 0) ? PUMP_DIR_BASE_TO_QUOTE
                                  : PUMP_DIR_QUOTE_TO_BASE;
        if (pred_pump != NULL && pool_idx == pred_idx) {
            ps = pred_pump;
        } else if (u->pump_live[pool_idx]) {
            ps = &u->pump[pool_idx];
        } else {
            return -1;
        }
        if (pump_quote_exact_in(ps, ain, pdir, &q) != 0 || !q.valid) {
            return -1;
        }
        *aout = q.amount_out;
        return 0;
    }
    return -1;
}

static int
quote_live_route(const live_univ_t *u, const compiled_route_t *r, uint64_t ain,
                 uint64_t *aout, uint32_t pred_idx,
                 const dlmm_pool_hot_t *pred_dlmm, const pump_state_t *pred_pump)
{
    uint64_t cur;
    uint8_t h;

    if (u == NULL || r == NULL || aout == NULL || !route_executable(r)
        || ain == 0) {
        return -1;
    }
    cur = ain;
    for (h = 0; h < r->n_hop; h++) {
        uint64_t nxt;
        if (quote_live_hop(u, r->pool[h], r->dir[h], cur, &nxt,
                           pred_idx, pred_dlmm, pred_pump) != 0) {
            return -1;
        }
        cur = nxt;
    }
    *aout = cur;
    return 0;
}

static void
size_executable(const live_univ_t *u, uint32_t pool_idx,
                uint32_t pred_idx, const dlmm_pool_hot_t *pred_dlmm,
                const pump_state_t *pred_pump, opportunity_t *best)
{
    const pool_meta_t *p;
    uint32_t i, s, k;

    if (u == NULL || best == NULL || pool_idx >= u->n) {
        return;
    }
    p = &u->routes.pool[pool_idx];
    for (i = 0; i < p->route_n; i++) {
        uint32_t rid = u->routes.rindex[p->route_off + i];
        const compiled_route_t *r;
        uint64_t aout, best_in = 0, best_out = 0;
        int have = 0;
        if (rid >= u->routes.n_route) {
            continue;
        }
        r = &u->routes.route[rid];
        if (!route_executable(r)) {
            continue;
        }
        for (s = 0; s < SIZE_LADDER_N; s++) {
            if (quote_live_route(u, r, SIZE_LADDER[s], &aout,
                                 pred_idx, pred_dlmm, pred_pump) != 0) {
                continue;
            }
            if (aout > SIZE_LADDER[s]
                && (!have || (aout - SIZE_LADDER[s]) > (best_out - best_in))) {
                best_in = SIZE_LADDER[s];
                best_out = aout;
                have = 1;
            }
        }
        if (!have) {
            continue;
        }
        {
            uint64_t lo = best_in / 2ull;
            uint64_t hi = best_in * 2ull;
            for (s = 0; s < SIZE_LADDER_N; s++) {
                if (SIZE_LADDER[s] == best_in) {
                    if (s > 0) {
                        lo = SIZE_LADDER[s - 1];
                    }
                    if (s + 1 < SIZE_LADDER_N) {
                        hi = SIZE_LADDER[s + 1];
                    }
                    break;
                }
            }
            if (lo < 1) {
                lo = 1;
            }
            if (hi > lo) {
                for (k = 1; k <= SIZE_REFINE_N; k++) {
                    uint64_t ain = lo + (hi - lo) * (uint64_t)k
                        / (uint64_t)(SIZE_REFINE_N + 1u);
                    if (ain == 0) {
                        continue;
                    }
                    if (quote_live_route(u, r, ain, &aout,
                                         pred_idx, pred_dlmm, pred_pump) != 0) {
                        continue;
                    }
                    if (aout > ain && (aout - ain) > (best_out - best_in)) {
                        best_in = ain;
                        best_out = aout;
                    }
                }
            }
        }
        consider(best, rid, best_in, best_out);
    }
}

static const uint8_t PUMP_BUY_DISC[8] = {
    0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
};
static const uint8_t PUMP_SELL_DISC[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};

int
hot_decode_trigger(uint8_t proto, const uint8_t *data, uint16_t len,
                   uint8_t swap_for_y, hot_trigger_t *out)
{
    if (out == NULL || data == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->protocol = proto;
    if (proto == PROTO_DLMM) {
        if (dlmm_decode_swap2(data, len, &out->dlmm) != 0) {
            return -1;
        }
        out->dlmm.swap_for_y = swap_for_y;
        return 0;
    }
    if (proto == PROTO_PUMP) {
        uint64_t ain;
        uint64_t mino;
        if (len < 24) {
            return -1;
        }
        memcpy(&ain, data + 8, 8);
        memcpy(&mino, data + 16, 8);
        out->pump.amount_in = ain;
        out->pump.min_amount_out = mino;
        if (memcmp(data, PUMP_BUY_DISC, 8) == 0) {
            out->pump.direction = PUMP_DIR_QUOTE_TO_BASE;
            return 0;
        }
        if (memcmp(data, PUMP_SELL_DISC, 8) == 0) {
            out->pump.direction = PUMP_DIR_BASE_TO_QUOTE;
            return 0;
        }
        return -1;
    }
    return -1;
}

static int
route0_partner(const live_univ_t *u, uint32_t hit, uint8_t want_proto,
               uint32_t *out_idx)
{
    const pool_meta_t *p;
    uint32_t i;

    if (u == NULL || hit >= u->n || out_idx == NULL) {
        return -1;
    }
    p = &u->routes.pool[hit];
    for (i = 0; i < p->route_n; i++) {
        uint32_t rid = u->routes.rindex[p->route_off + i];
        const compiled_route_t *r;
        uint8_t h;
        if (rid >= u->routes.n_route) {
            continue;
        }
        r = &u->routes.route[rid];
        if (r->family != ROUTE_FAM_0_DLMM_PUMP) {
            continue;
        }
        for (h = 0; h < r->n_hop; h++) {
            uint32_t pj = r->pool[h];
            if (pj != hit && pj < u->n && u->meta[pj].protocol == want_proto) {
                *out_idx = pj;
                return 0;
            }
        }
    }
    return -1;
}

static void
keep_better(opportunity_t *best, const opportunity_t *cand)
{
    if (cand == NULL || !cand->valid) {
        return;
    }
    if (best->valid
        && (cand->gross_profit < best->gross_profit
            || (cand->gross_profit == best->gross_profit
                && cand->amount_in >= best->amount_in))) {
        return;
    }
    *best = *cand;
}

int
hot_decide(const live_univ_t *u, uint32_t pool_idx,
           const hot_trigger_t *n, hot_decision_t *out)
{
    opportunity_t best;
    uint32_t partner;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (u == NULL || n == NULL || pool_idx >= u->n) {
        return -1;
    }
    out->state_version = u->state_version;
    memset(&best, 0, sizeof(best));

    if (u->meta[pool_idx].protocol == PROTO_DLMM && n->protocol == PROTO_DLMM) {
        dlmm_pool_hot_t spec;
        dlmm_apply_result_t res;
        dlmm_state_t s_prime;
        if (dlmm_cache_predict(&u->dlmm, pool_idx, &n->dlmm, &spec, &res) != 0) {
            return 0;
        }
        dlmm_hot_to_state(&spec, &s_prime);
        if (route0_partner(u, pool_idx, PROTO_PUMP, &partner) == 0
            && u->pump_live[partner]) {
            if (cycle_size(&s_prime, &u->pump[partner], &best) != 0) {
                return -1;
            }
            if (best.valid) {
                best.route_id = ROUTE_DLMM_PUMP;
            }
        }
        size_executable(u, pool_idx, pool_idx, &spec, NULL, &best);
    } else if (u->meta[pool_idx].protocol == PROTO_PUMP
               && n->protocol == PROTO_PUMP) {
        pump_state_t s_prime;
        pump_swap_result_t res;
        const dlmm_pool_hot_t *h;
        dlmm_state_t s_dlmm;
        if (!u->pump_live[pool_idx]) {
            return 0;
        }
        if (pump_apply_swap(&u->pump[pool_idx], &n->pump, &s_prime, &res) != 0) {
            return 0;
        }
        if (route0_partner(u, pool_idx, PROTO_DLMM, &partner) == 0) {
            h = dlmm_cache_get(&u->dlmm, partner);
            if (h != NULL) {
                dlmm_hot_to_state(h, &s_dlmm);
                if (cycle_size(&s_dlmm, &s_prime, &best) != 0) {
                    return -1;
                }
                if (best.valid) {
                    best.route_id = ROUTE_DLMM_PUMP;
                }
            }
        }
        size_executable(u, pool_idx, pool_idx, NULL, &s_prime, &best);
    } else {
        return 0;
    }

    keep_better(&out->opp, &best);
    return 0;
}

int
hot_eval_live(const live_univ_t *u, uint32_t pool_idx,
              const hot_trigger_t *n, opportunity_t *out)
{
    hot_decision_t d;
    int rc;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    rc = hot_decide(u, pool_idx, n, &d);
    if (rc != 0) {
        return rc;
    }
    *out = d.opp;
    return 0;
}

int
hot_commit(live_univ_t *u, uint32_t pool_idx, const hot_trigger_t *n)
{
    if (u == NULL || n == NULL || pool_idx >= u->n) {
        return -1;
    }
    if (u->meta[pool_idx].protocol == PROTO_DLMM && n->protocol == PROTO_DLMM) {
        dlmm_pool_hot_t spec;
        dlmm_apply_result_t res;
        if (dlmm_cache_predict(&u->dlmm, pool_idx, &n->dlmm, &spec, &res) != 0) {
            return -1;
        }
        if (dlmm_cache_commit(&u->dlmm, pool_idx, &spec) != 0) {
            return -1;
        }
    } else if (u->meta[pool_idx].protocol == PROTO_PUMP
               && n->protocol == PROTO_PUMP) {
        pump_state_t after;
        pump_swap_result_t res;
        if (!u->pump_live[pool_idx]) {
            return -1;
        }
        if (pump_apply_swap(&u->pump[pool_idx], &n->pump, &after, &res) != 0) {
            return -1;
        }
        u->pump[pool_idx] = after;
    } else {
        return -1;
    }
    u->state_version++;
    return 0;
}
