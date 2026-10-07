#include "paper.h"
#include "cycle.h"

#include <string.h>

typedef struct {
    uint32_t              pool_idx;
    const dlmm_pool_hot_t *dlmm;
    const pump_state_t    *pump;
} paper_pred_t;

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

static const dlmm_pool_hot_t *
pred_dlmm(const live_univ_t *u, uint32_t pool_idx, const paper_pred_t *pred)
{
    if (pred != NULL && pred->dlmm != NULL && pred->pool_idx == pool_idx) {
        return pred->dlmm;
    }
    return dlmm_cache_get(&u->dlmm, pool_idx);
}

static const pump_state_t *
pred_pump(const live_univ_t *u, uint32_t pool_idx, const paper_pred_t *pred)
{
    if (pred != NULL && pred->pump != NULL && pred->pool_idx == pool_idx) {
        return pred->pump;
    }
    return (u->pump_live != NULL && u->pump_live[pool_idx]) ? &u->pump[pool_idx]
                                                            : NULL;
}

static int
quote_hop_pred(const live_univ_t *u, const univ_state_t *st,
               uint32_t pool_idx, int dir, uint64_t ain, uint64_t *aout,
               const paper_pred_t *pred)
{
    const live_meta_t *m;

    if (u == NULL || aout == NULL || pool_idx >= u->n || ain == 0) {
        return -1;
    }
    m = &u->meta[pool_idx];
    if (m->protocol == PROTO_DLMM) {
        const dlmm_pool_hot_t *h = pred_dlmm(u, pool_idx, pred);
        dlmm_state_t s;
        dlmm_quote_t q;
        uint8_t swap_for_y = (dir == 0) ? 1u : 0u;
        if (h == NULL) {
            return -1;
        }
        dlmm_hot_to_state(h, &s);
        if (dlmm_quote_exact_in(&s, ain, swap_for_y, &q) != 0 || !q.valid) {
            return -1;
        }
        *aout = q.amount_out;
        return 0;
    }
    if (m->protocol == PROTO_PUMP) {
        const pump_state_t *ps = pred_pump(u, pool_idx, pred);
        pump_quote_t q;
        uint8_t pdir = (dir == 0) ? PUMP_DIR_BASE_TO_QUOTE : PUMP_DIR_QUOTE_TO_BASE;
        if (ps == NULL
            || pump_quote_exact_in(ps, ain, pdir, &q) != 0 || !q.valid) {
            return -1;
        }
        *aout = q.amount_out;
        return 0;
    }
    if (st == NULL || pool_idx >= st->n || !u->extra_live[pool_idx]) {
        return -1;
    }
    if (m->protocol == PROTO_CLMM || m->protocol == PROTO_ORCA) {
        return clmm_quote(&st->clmm[pool_idx], ain, dir, aout) == ADAPTER_OK ? 0 : -1;
    }
    if (m->protocol == PROTO_CPMM || m->protocol == PROTO_DAMM) {
        return amm2_quote(&st->amm2[pool_idx], ain, dir, aout) == ADAPTER_OK ? 0 : -1;
    }
    return -1;
}

int
paper_quote_hop(const live_univ_t *u, const univ_state_t *st,
                uint32_t pool_idx, int dir, uint64_t ain, uint64_t *aout)
{
    return quote_hop_pred(u, st, pool_idx, dir, ain, aout, NULL);
}

static int
quote_route_pred(const live_univ_t *u, const univ_state_t *st,
                 uint32_t route_idx, uint64_t amount_in, uint64_t *amount_out,
                 const paper_pred_t *pred)
{
    const compiled_route_t *r;
    uint64_t cur;
    uint8_t h;

    if (u == NULL || amount_out == NULL || route_idx >= u->routes.n_route) {
        return -1;
    }
    r = &u->routes.route[route_idx];
    if (r->n_hop < 2 || amount_in == 0) {
        return -1;
    }
    cur = amount_in;
    for (h = 0; h < r->n_hop; h++) {
        uint64_t nxt;
        if (quote_hop_pred(u, st, r->pool[h], r->dir[h], cur, &nxt, pred) != 0) {
            return -1;
        }
        cur = nxt;
    }
    *amount_out = cur;
    return 0;
}

int
paper_quote_route(const live_univ_t *u, const univ_state_t *st,
                  uint32_t route_idx, uint64_t amount_in,
                  uint64_t *amount_out)
{
    return quote_route_pred(u, st, route_idx, amount_in, amount_out, NULL);
}

static void
keep(paper_opp_t *best, uint32_t rid, const compiled_route_t *r,
     uint64_t ain, uint64_t aout, uint8_t state_ok)
{
    uint64_t gp;
    uint8_t fam;

    if (aout <= ain) {
        return;
    }
    gp = aout - ain;
    if (best->opp.valid
        && (gp < best->opp.gross_profit
            || (gp == best->opp.gross_profit && ain >= best->opp.amount_in))) {
        return;
    }
    fam = r->family;
    best->opp.route_id = rid;
    best->opp.amount_in = ain;
    best->opp.amount_out = aout;
    best->opp.gross_profit = gp;
    best->opp.direction = 0;
    best->opp.valid = 1;
    best->route_id = rid;
    best->family = fam;
    best->n_hop = r->n_hop;
    memcpy(best->proto, r->proto, UNIV_HOP_MAX);
    best->searchable = 1;
    best->state_ok = state_ok;
    if (route_executable(r)) {
        best->signed_ready = 1;
        best->reason = PAPER_SIGNED;
    } else {
        best->signed_ready = 0;
        best->reason = PAPER_EXEC_FAMILY_MISSING;
    }
}

static int
size_route(const live_univ_t *u, const univ_state_t *st,
           uint32_t rid, const compiled_route_t *r, paper_opp_t *best,
           const paper_pred_t *pred)
{
    uint64_t aout;
    uint64_t best_in = 0;
    uint64_t best_out = 0;
    int have = 0;
    uint32_t s;
    uint32_t k;

    for (s = 0; s < SIZE_LADDER_N; s++) {
        if (quote_route_pred(u, st, rid, SIZE_LADDER[s], &aout, pred) != 0) {
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
        return 0;
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
                if (quote_route_pred(u, st, rid, ain, &aout, pred) != 0) {
                    continue;
                }
                if (aout > ain && (aout - ain) > (best_out - best_in)) {
                    best_in = ain;
                    best_out = aout;
                }
            }
        }
    }
    keep(best, rid, r, best_in, best_out, 1);
    return 1;
}

static int
eval_pool_pred(const live_univ_t *u, const univ_state_t *st,
               uint32_t pool_idx, paper_opp_t *best, const paper_pred_t *pred)
{
    const pool_meta_t *p;
    uint32_t i;

    if (best == NULL) {
        return -1;
    }
    memset(best, 0, sizeof(*best));
    best->pool_idx = pool_idx;
    best->reason = PAPER_STATE_INSUFFICIENT;
    if (u == NULL || pool_idx >= u->n) {
        return -1;
    }
    p = &u->routes.pool[pool_idx];
    for (i = 0; i < p->route_n; i++) {
        uint32_t rid = u->routes.rindex[p->route_off + i];
        const compiled_route_t *r;
        if (rid >= u->routes.n_route) {
            continue;
        }
        r = &u->routes.route[rid];
        if (r->family == ROUTE_FAM_0_DLMM_PUMP && r->n_hop == 2) {
            uint32_t a = r->pool[0];
            uint32_t b = r->pool[1];
            const dlmm_state_t *ds = NULL;
            const pump_state_t *ps = NULL;
            dlmm_state_t tmp;
            const dlmm_pool_hot_t *h;
            const pump_state_t *pp;
            opportunity_t cyc;
            if (a >= u->n || b >= u->n) {
                continue;
            }
            if (u->meta[a].protocol == PROTO_DLMM && u->meta[b].protocol == PROTO_PUMP) {
                h = pred_dlmm(u, a, pred);
                pp = pred_pump(u, b, pred);
                if (h != NULL && pp != NULL) {
                    dlmm_hot_to_state(h, &tmp);
                    ds = &tmp;
                    ps = pp;
                }
            } else if (u->meta[a].protocol == PROTO_PUMP && u->meta[b].protocol == PROTO_DLMM) {
                h = pred_dlmm(u, b, pred);
                pp = pred_pump(u, a, pred);
                if (h != NULL && pp != NULL) {
                    dlmm_hot_to_state(h, &tmp);
                    ds = &tmp;
                    ps = pp;
                }
            }
            if (ds != NULL && ps != NULL && cycle_size(ds, ps, &cyc) == 0 && cyc.valid) {
                keep(best, rid, r, cyc.amount_in, cyc.amount_out, 1);
                best->opp.direction = cyc.direction;
            }
            continue;
        }
        (void)size_route(u, st, rid, r, best, pred);
    }
    if (!best->opp.valid) {
        best->searchable = 0;
        best->reason = PAPER_STATE_INSUFFICIENT;
    }
    return 0;
}

int
paper_eval_pool(const live_univ_t *u, const univ_state_t *st,
                uint32_t pool_idx, paper_opp_t *best)
{
    return eval_pool_pred(u, st, pool_idx, best, NULL);
}

int
paper_eval_trigger(const live_univ_t *u, univ_state_t *st,
                   uint32_t pool_idx, const hot_trigger_t *n,
                   paper_opp_t *best)
{
    paper_pred_t pred;
    dlmm_pool_hot_t spec;
    pump_state_t after;
    int used_pred = 0;

    if (best == NULL) {
        return -1;
    }
    memset(best, 0, sizeof(*best));
    memset(&pred, 0, sizeof(pred));
    pred.pool_idx = pool_idx;
    if (u == NULL || pool_idx >= u->n) {
        return -1;
    }
    /* No N → fail closed. Canonical S without the trigger is the $0.61 phantom. */
    if (n == NULL) {
        best->reason = PAPER_STATE_INSUFFICIENT;
        return 0;
    }
    if (n->protocol == PROTO_DLMM
        && u->meta[pool_idx].protocol == PROTO_DLMM) {
        dlmm_apply_result_t res;
        if (dlmm_cache_predict(&u->dlmm, pool_idx, &n->dlmm, &spec, &res) == 0) {
            pred.dlmm = &spec;
            used_pred = 1;
        }
    } else if (n->protocol == PROTO_PUMP
               && u->meta[pool_idx].protocol == PROTO_PUMP
               && u->pump_live[pool_idx]) {
        pump_swap_result_t res;
        if (pump_apply_swap(&u->pump[pool_idx], &n->pump, &after, &res) == 0) {
            pred.pump = &after;
            used_pred = 1;
        }
    }
    if (!used_pred) {
        best->reason = PAPER_STATE_INSUFFICIENT;
        return 0;
    }
    return eval_pool_pred(u, st, pool_idx, best, &pred);
}
