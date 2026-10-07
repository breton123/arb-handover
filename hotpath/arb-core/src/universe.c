#include "universe.h"

#include <stdlib.h>
#include <string.h>

#define MINT_TAB  4096u

typedef struct {
    uint32_t pool;
    uint8_t  dir;   /* 0 = this mint is X (out along X→Y), 1 = mint is Y */
} edge_t;

typedef struct {
    uint8_t  mint[32];
    uint32_t off;
    uint16_t n;
    uint8_t  used;
} mint_slot_t;

int
route_hops_supported(const compiled_route_t *r)
{
    uint8_t i;

    if (r == NULL || (r->n_hop != 2 && r->n_hop != 3)) {
        return 0;
    }
    for (i = 0; i < r->n_hop; i++) {
        uint32_t bit = proto_rel_bit(r->proto[i]);
        if (bit == 0 || (bit & ROUTE_VENUE_SUPPORTED) == 0) {
            return 0;
        }
    }
    return 1;
}

int
route_executable(const compiled_route_t *r)
{
    return route_hops_supported(r);
}

uint8_t
route_family_of(const compiled_route_t *r)
{
    uint32_t bits = 0;
    uint8_t i;

    if (r == NULL || r->n_hop < 2) {
        return ROUTE_FAM_OTHER;
    }
    for (i = 0; i < r->n_hop; i++) {
        bits |= proto_rel_bit(r->proto[i]);
    }
    /* Mixed 2-hop DLMM+Pump keeps FAM_0; push_route may demote to FAM_5. */
    if (r->n_hop == 2 && bits == (REL_N_DLMM | REL_N_PUMP)) {
        return ROUTE_FAM_0_DLMM_PUMP;
    }
    /* Any other closed 2/3-hop over {DLMM,Pump} is executable closure. */
    if (route_hops_supported(r)) {
        return ROUTE_FAM_6_SUPPORTED;
    }
    if (r->n_hop == 2 && bits == (REL_N_CLMM | REL_N_DLMM)) {
        return ROUTE_FAM_1_CLMM_DLMM;
    }
    if (r->n_hop == 2 && bits == (REL_N_CPMM | REL_N_DLMM)) {
        return ROUTE_FAM_2_CPMM_DLMM;
    }
    if (r->n_hop == 2 && bits == (REL_N_DLMM | REL_N_DAMM)) {
        return ROUTE_FAM_3_DLMM_DAMM;
    }
    if (r->n_hop == 2 && bits == (REL_N_ORCA | REL_N_DLMM)) {
        return ROUTE_FAM_4_ORCA_DLMM;
    }
    return ROUTE_FAM_OTHER;
}

int
universe_init(universe_t *u, uint32_t pool_cap, uint32_t route_cap)
{
    uint32_t rcap;

    if (u == NULL || pool_cap == 0 || route_cap == 0) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    rcap = route_cap * 3u;
    if (rcap < route_cap) {
        return -1;
    }
    u->pool = calloc(pool_cap, sizeof(*u->pool));
    u->route = calloc(route_cap, sizeof(*u->route));
    u->rindex = calloc(rcap, sizeof(*u->rindex));
    if (u->pool == NULL || u->route == NULL || u->rindex == NULL) {
        universe_free(u);
        return -1;
    }
    u->pool_cap = pool_cap;
    u->route_cap = route_cap;
    u->rindex_cap = rcap;
    return 0;
}

void
universe_free(universe_t *u)
{
    if (u == NULL) {
        return;
    }
    free(u->pool);
    free(u->route);
    free(u->rindex);
    memset(u, 0, sizeof(*u));
}

int
universe_add_pool(universe_t *u, uint8_t proto,
                  const uint8_t mint_x[32], const uint8_t mint_y[32])
{
    pool_meta_t *p;

    if (u == NULL || mint_x == NULL || mint_y == NULL || proto == 0) {
        return -1;
    }
    if (u->n_pool >= u->pool_cap) {
        return -1;
    }
    if (memcmp(mint_x, mint_y, 32) == 0) {
        return -1;
    }
    p = &u->pool[u->n_pool];
    memset(p, 0, sizeof(*p));
    p->protocol = proto;
    memcpy(p->mint_x, mint_x, 32);
    memcpy(p->mint_y, mint_y, 32);
    p->state_idx = u->n_pool;
    u->n_pool++;
    return 0;
}

static uint32_t
mint_hash(const uint8_t m[32])
{
    uint64_t x;
    memcpy(&x, m, 8);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    return (uint32_t)x;
}

static int
mint_put(mint_slot_t *tab, const uint8_t m[32], uint32_t *idx)
{
    uint32_t mask = MINT_TAB - 1u;
    uint32_t j = mint_hash(m) & mask;
    uint32_t d;

    for (d = 0; d < MINT_TAB; d++) {
        if (!tab[j].used) {
            memcpy(tab[j].mint, m, 32);
            tab[j].used = 1;
            tab[j].off = 0;
            tab[j].n = 0;
            *idx = j;
            return 1;
        }
        if (memcmp(tab[j].mint, m, 32) == 0) {
            *idx = j;
            return 0;
        }
        j = (j + 1u) & mask;
    }
    return -1;
}

int
route0_pair_compatible(const uint8_t dlmm_x[32], const uint8_t dlmm_y[32],
                       const uint8_t pump_base[32],
                       const uint8_t pump_quote[32])
{
    if (dlmm_x == NULL || dlmm_y == NULL || pump_base == NULL
        || pump_quote == NULL) {
        return 0;
    }
    if (memcmp(dlmm_y, MINT_SOL, 32) != 0
        || memcmp(pump_quote, MINT_SOL, 32) != 0) {
        return 0;
    }
    if (memcmp(dlmm_x, MINT_SOL, 32) == 0
        || memcmp(pump_base, MINT_SOL, 32) == 0) {
        return 0;
    }
    return memcmp(dlmm_x, pump_base, 32) == 0;
}

static int
route0_ok_or_not_route0(const universe_t *u, const compiled_route_t *r)
{
    const pool_meta_t *d = NULL;
    const pool_meta_t *p = NULL;
    uint8_t h;

    if (r == NULL || route_family_of(r) != ROUTE_FAM_0_DLMM_PUMP) {
        return 1;
    }
    for (h = 0; h < r->n_hop; h++) {
        uint32_t i = r->pool[h];
        if (i >= u->n_pool) {
            return 0;
        }
        if (u->pool[i].protocol == PROTO_DLMM) {
            d = &u->pool[i];
        } else if (u->pool[i].protocol == PROTO_PUMP) {
            p = &u->pool[i];
        }
    }
    if (d == NULL || p == NULL) {
        return 0;
    }
    return route0_pair_compatible(d->mint_x, d->mint_y, p->mint_x, p->mint_y);
}

static int
push_route(universe_t *u, compiled_route_t *r)
{
    if (u == NULL || r == NULL) {
        return -1;
    }
    r->family = route_family_of(r);
    if (r->family == ROUTE_FAM_0_DLMM_PUMP && !route0_ok_or_not_route0(u, r)) {
        r->family = ROUTE_FAM_5_DLMM_PUMP_TYPED;
    }
    if (u->n_route >= u->route_cap) {
        return -1;
    }
    u->route[u->n_route] = *r;
    u->n_route++;
    return 0;
}

static int
allowed(uint8_t proto, uint32_t mask)
{
    return (proto_rel_bit(proto) & mask) != 0;
}

int
universe_compile(universe_t *u, uint32_t proto_mask)
{
    mint_slot_t tab[MINT_TAB];
    edge_t *edges = NULL;
    uint32_t ecap;
    uint32_t i;
    uint32_t rfill = 0;
    int rc = 0;

    if (u == NULL || proto_mask == 0) {
        return -1;
    }
    u->n_route = 0;
    u->proto_mask = proto_mask;
    memset(tab, 0, sizeof(tab));
    ecap = u->n_pool * 2u;
    if (ecap == 0) {
        return 0;
    }
    edges = calloc(ecap, sizeof(*edges));
    if (edges == NULL) {
        return -1;
    }

    for (i = 0; i < u->n_pool; i++) {
        uint32_t ix, iy;
        const pool_meta_t *p = &u->pool[i];
        if (!allowed(p->protocol, proto_mask)) {
            continue;
        }
        if (mint_put(tab, p->mint_x, &ix) < 0 || mint_put(tab, p->mint_y, &iy) < 0) {
            rc = -1;
            goto done;
        }
        tab[ix].n++;
        tab[iy].n++;
    }
    {
        uint32_t off = 0;
        for (i = 0; i < MINT_TAB; i++) {
            if (tab[i].used) {
                tab[i].off = off;
                off += tab[i].n;
                tab[i].n = 0;
            }
        }
        if (off > ecap) {
            rc = -1;
            goto done;
        }
    }
    for (i = 0; i < u->n_pool; i++) {
        uint32_t ix, iy;
        const pool_meta_t *p = &u->pool[i];
        if (!allowed(p->protocol, proto_mask)) {
            continue;
        }
        (void)mint_put(tab, p->mint_x, &ix);
        (void)mint_put(tab, p->mint_y, &iy);
        edges[tab[ix].off + tab[ix].n].pool = i;
        edges[tab[ix].off + tab[ix].n].dir = 0;
        tab[ix].n++;
        edges[tab[iy].off + tab[iy].n].pool = i;
        edges[tab[iy].off + tab[iy].n].dir = 1;
        tab[iy].n++;
    }

    /* 2-hop: SOL → T → SOL */
    {
        uint32_t sidx;
        uint32_t a, b;
        if (mint_put(tab, MINT_SOL, &sidx) < 0 || !tab[sidx].used) {
            goto index;
        }
        for (a = 0; a < tab[sidx].n; a++) {
            edge_t e1 = edges[tab[sidx].off + a];
            const pool_meta_t *p1 = &u->pool[e1.pool];
            const uint8_t *t = e1.dir == 0 ? p1->mint_y : p1->mint_x;
            uint32_t tidx;
            if (memcmp(t, MINT_SOL, 32) == 0) {
                continue;
            }
            if (mint_put(tab, t, &tidx) < 0) {
                continue;
            }
            for (b = 0; b < tab[tidx].n; b++) {
                edge_t e2 = edges[tab[tidx].off + b];
                const pool_meta_t *p2 = &u->pool[e2.pool];
                const uint8_t *back;
                compiled_route_t r;
                if (e2.pool == e1.pool) {
                    continue;
                }
                back = e2.dir == 0 ? p2->mint_y : p2->mint_x;
                if (memcmp(back, MINT_SOL, 32) != 0) {
                    continue;
                }
                memset(&r, 0, sizeof(r));
                r.n_hop = 2;
                r.proto[0] = p1->protocol;
                r.proto[1] = p2->protocol;
                r.pool[0] = e1.pool;
                r.pool[1] = e2.pool;
                r.dir[0] = e1.dir;
                r.dir[1] = e2.dir;
                if (push_route(u, &r) != 0) {
                    rc = -1;
                    goto done;
                }
            }
        }

        /* 3-hop: SOL → T1 → T2 → SOL */
        for (a = 0; a < tab[sidx].n; a++) {
            edge_t e1 = edges[tab[sidx].off + a];
            const pool_meta_t *p1 = &u->pool[e1.pool];
            const uint8_t *t1 = e1.dir == 0 ? p1->mint_y : p1->mint_x;
            uint32_t t1idx;
            uint32_t b;
            if (memcmp(t1, MINT_SOL, 32) == 0) {
                continue;
            }
            if (mint_put(tab, t1, &t1idx) < 0) {
                continue;
            }
            for (b = 0; b < tab[t1idx].n; b++) {
                edge_t e2 = edges[tab[t1idx].off + b];
                const pool_meta_t *p2 = &u->pool[e2.pool];
                const uint8_t *t2;
                uint32_t t2idx;
                uint32_t c;
                if (e2.pool == e1.pool) {
                    continue;
                }
                t2 = e2.dir == 0 ? p2->mint_y : p2->mint_x;
                if (memcmp(t2, MINT_SOL, 32) == 0 || memcmp(t2, t1, 32) == 0) {
                    continue;
                }
                if (mint_put(tab, t2, &t2idx) < 0) {
                    continue;
                }
                for (c = 0; c < tab[t2idx].n; c++) {
                    edge_t e3 = edges[tab[t2idx].off + c];
                    const pool_meta_t *p3 = &u->pool[e3.pool];
                    const uint8_t *back;
                    compiled_route_t r;
                    if (e3.pool == e1.pool || e3.pool == e2.pool) {
                        continue;
                    }
                    back = e3.dir == 0 ? p3->mint_y : p3->mint_x;
                    if (memcmp(back, MINT_SOL, 32) != 0) {
                        continue;
                    }
                    memset(&r, 0, sizeof(r));
                    r.n_hop = 3;
                    r.proto[0] = p1->protocol;
                    r.proto[1] = p2->protocol;
                    r.proto[2] = p3->protocol;
                    r.pool[0] = e1.pool;
                    r.pool[1] = e2.pool;
                    r.pool[2] = e3.pool;
                    r.dir[0] = e1.dir;
                    r.dir[1] = e2.dir;
                    r.dir[2] = e3.dir;
                    if (push_route(u, &r) != 0) {
                        rc = -1;
                        goto done;
                    }
                }
            }
        }
    }

index:
    for (i = 0; i < u->n_pool; i++) {
        u->pool[i].route_off = 0;
        u->pool[i].route_n = 0;
    }
    {
        uint32_t *count = calloc(u->n_pool, sizeof(*count));
        uint32_t r;
        if (count == NULL) {
            rc = -1;
            goto done;
        }
        for (r = 0; r < u->n_route; r++) {
            uint8_t h;
            for (h = 0; h < u->route[r].n_hop; h++) {
                uint32_t p = u->route[r].pool[h];
                if (p < u->n_pool) {
                    count[p]++;
                }
            }
        }
        rfill = 0;
        for (i = 0; i < u->n_pool; i++) {
            u->pool[i].route_off = rfill;
            rfill += count[i];
            u->pool[i].route_n = 0;
        }
        if (rfill > u->rindex_cap) {
            free(count);
            rc = -1;
            goto done;
        }
        for (r = 0; r < u->n_route; r++) {
            uint8_t h;
            for (h = 0; h < u->route[r].n_hop; h++) {
                uint32_t p = u->route[r].pool[h];
                uint32_t slot;
                if (p >= u->n_pool) {
                    continue;
                }
                slot = u->pool[p].route_off + u->pool[p].route_n;
                u->rindex[slot] = r;
                u->pool[p].route_n++;
            }
        }
        free(count);
    }

done:
    free(edges);
    return rc;
}

int
univ_state_init(univ_state_t *st, uint32_t n)
{
    if (st == NULL || n == 0) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->amm2 = calloc(n, sizeof(*st->amm2));
    st->clmm = calloc(n, sizeof(*st->clmm));
    if (st->amm2 == NULL || st->clmm == NULL) {
        univ_state_free(st);
        return -1;
    }
    st->n = n;
    return 0;
}

void
univ_state_free(univ_state_t *st)
{
    if (st == NULL) {
        return;
    }
    free(st->amm2);
    free(st->clmm);
    memset(st, 0, sizeof(*st));
}

void
universe_fill_synthetic(const universe_t *u, univ_state_t *st)
{
    uint32_t i;

    if (u == NULL || st == NULL) {
        return;
    }
    for (i = 0; i < u->n_pool && i < st->n; i++) {
        uint8_t proto = u->pool[i].protocol;
        st->amm2[i].reserve_x = 100ull * 1000000000ull;
        st->amm2[i].reserve_y = 200ull * 1000000000ull;
        st->amm2[i].fee_bps = 25;
        st->amm2[i].status = 1;
        st->clmm[i].tick = 0;
        st->clmm[i].sqrt_price_x64 = 1ull << 32; /* ~1.0 in Q64 after shift */
        st->clmm[i].sqrt_price_x64 = 1ull << 63;
        st->clmm[i].liquidity = 1ull << 40;
        st->clmm[i].fee_bps = 25;
        st->clmm[i].status = 1;
        st->clmm[i].has_ticks = (uint8_t)(proto == PROTO_CLMM || proto == PROTO_ORCA);
    }
}

static void
token_mint(uint8_t out[32], uint32_t id)
{
    memset(out, 0, 32);
    out[0] = 0xa1;
    out[1] = 0xb2;
    memcpy(out + 28, &id, 4);
}

int
universe_seed(universe_t *u, uint32_t n_ids)
{
    static const uint8_t PROTOS[CLASSIFY_N_MAX] = {
        PROTO_DLMM, PROTO_PUMP, PROTO_CLMM, PROTO_CPMM, PROTO_DAMM, PROTO_ORCA
    };
    uint32_t t;
    uint32_t p;
    uint8_t tok[8][32];

    if (u == NULL || n_ids < 2 || n_ids > CLASSIFY_N_MAX) {
        return -1;
    }
    for (t = 0; t < 8; t++) {
        token_mint(tok[t], t + 1u);
    }
    for (p = 0; p < n_ids; p++) {
        for (t = 0; t < 8; t++) {
            if (universe_add_pool(u, PROTOS[p], MINT_SOL, tok[t]) != 0) {
                return -1;
            }
        }
    }
    /* TOKEN-TOKEN bridges so 3-hop SOL→T0→T1→SOL exists. */
    for (t = 0; t < 7; t++) {
        if (universe_add_pool(u, PROTO_DLMM, tok[t], tok[t + 1u]) != 0) {
            return -1;
        }
    }
    return 0;
}

int
universe_scale_dummy(universe_t *u, uint32_t n_dummy)
{
    uint32_t *keep;
    uint32_t kn;
    uint32_t i;
    uint32_t need;

    if (u == NULL || u->n_pool < 2) {
        return -1;
    }
    kn = u->pool[0].route_n;
    keep = NULL;
    if (kn > 0) {
        keep = malloc(kn * sizeof(*keep));
        if (keep == NULL) {
            return -1;
        }
        memcpy(keep, u->rindex + u->pool[0].route_off, kn * sizeof(*keep));
    }
    need = kn + n_dummy;
    if (need > u->rindex_cap || u->n_route + n_dummy > u->route_cap) {
        free(keep);
        return -1;
    }
    if (kn > 0) {
        memcpy(u->rindex, keep, kn * sizeof(*keep));
    }
    free(keep);
    u->pool[0].route_off = 0;
    u->pool[0].route_n = kn;
    u->pool[1].route_off = kn;
    u->pool[1].route_n = 0;
    for (i = 0; i < n_dummy; i++) {
        compiled_route_t r;
        memset(&r, 0, sizeof(r));
        r.n_hop = 2;
        r.proto[0] = PROTO_DLMM;
        r.proto[1] = PROTO_PUMP;
        r.pool[0] = 1;
        r.pool[1] = 1;
        r.dir[0] = 0;
        r.dir[1] = 1;
        r.family = ROUTE_FAM_OTHER;
        u->route[u->n_route] = r;
        u->rindex[u->pool[1].route_off + u->pool[1].route_n] = u->n_route;
        u->pool[1].route_n++;
        u->n_route++;
    }
    return 0;
}
