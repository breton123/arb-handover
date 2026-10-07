#include "live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
read_n(FILE *f, void *p, size_t n)
{
    return fread(p, 1, n, f) == n ? 0 : -1;
}

static int
read_u8(FILE *f, uint8_t *v)
{
    return read_n(f, v, 1);
}

static int
read_u16(FILE *f, uint16_t *v)
{
    return read_n(f, v, 2);
}

static int
read_u32(FILE *f, uint32_t *v)
{
    return read_n(f, v, 4);
}

static int
read_u64(FILE *f, uint64_t *v)
{
    return read_n(f, v, 8);
}

static uint32_t
route_cap_for(uint32_t n)
{
    uint64_t rc = (uint64_t)n * 64ull;

    if (rc < 65536ull) {
        rc = 65536ull;
    }
    if (rc > (uint64_t)UNIV_ROUTE_HARD_MAX) {
        rc = UNIV_ROUTE_HARD_MAX;
    }
    return (uint32_t)rc;
}

static void *
grow_zero(void *p, uint32_t old_n, uint32_t new_n, size_t elem)
{
    uint8_t *q = realloc(p, (size_t)new_n * elem);

    if (q == NULL) {
        return NULL;
    }
    if (new_n > old_n) {
        memset(q + (size_t)old_n * elem, 0, (size_t)(new_n - old_n) * elem);
    }
    return q;
}

int
live_univ_init(live_univ_t *u)
{
    if (u == NULL) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    dlmm_cache_init(&u->dlmm);
    return 0;
}

int
live_univ_reserve(live_univ_t *u, uint32_t cap)
{
    uint32_t key_cap;

    if (u == NULL || cap == 0 || cap > UNIV_POOL_HARD_MAX) {
        return -1;
    }
    if (u->cap >= cap && u->meta != NULL) {
        return 0;
    }
    u->meta = grow_zero(u->meta, u->cap, cap, sizeof(*u->meta));
    u->pump = grow_zero(u->pump, u->cap, cap, sizeof(*u->pump));
    u->pump_live = grow_zero(u->pump_live, u->cap, cap, sizeof(*u->pump_live));
    u->amm2 = grow_zero(u->amm2, u->cap, cap, sizeof(*u->amm2));
    u->clmm = grow_zero(u->clmm, u->cap, cap, sizeof(*u->clmm));
    u->extra_live = grow_zero(u->extra_live, u->cap, cap, sizeof(*u->extra_live));
    if (u->meta == NULL || u->pump == NULL || u->pump_live == NULL
        || u->amm2 == NULL || u->clmm == NULL || u->extra_live == NULL) {
        return -1;
    }
    if (dlmm_cache_reserve(&u->dlmm, cap) != 0) {
        return -1;
    }
    key_cap = cap * 2u;
    if (key_cap < 1024u) {
        key_cap = 1024u;
    }
    if (u->keys.slot == NULL) {
        if (pool_table_init(&u->keys, key_cap) != 0) {
            return -1;
        }
    }
    if (u->routes.pool == NULL) {
        if (universe_init(&u->routes, cap, route_cap_for(cap)) != 0) {
            return -1;
        }
    }
    u->cap = cap;
    return 0;
}

void
live_univ_free(live_univ_t *u)
{
    if (u == NULL) {
        return;
    }
    free(u->meta);
    free(u->pump);
    free(u->pump_live);
    free(u->amm2);
    free(u->clmm);
    free(u->extra_live);
    dlmm_cache_free(&u->dlmm);
    pool_table_free(&u->keys);
    universe_free(&u->routes);
    memset(u, 0, sizeof(*u));
}

static int
load_dlmm(FILE *f, live_univ_t *u, uint32_t idx)
{
    dlmm_state_t s;
    uint16_t nbin;
    uint16_t i;
    int32_t active;
    uint16_t bin_step;
    uint8_t status;
    uint16_t bf, fp, dp, rf, pshare;
    uint32_t vfc, mva;
    uint8_t power, fee_mode;
    uint32_t vacc, vref;
    int32_t iref;
    int64_t last, now;
    uint64_t rx, ry;

    memset(&s, 0, sizeof(s));
    if (read_n(f, &active, 4) != 0 || read_u16(f, &bin_step) != 0
        || read_u8(f, &status) != 0) {
        return -1;
    }
    if (read_u16(f, &bf) != 0 || read_u16(f, &fp) != 0 || read_u16(f, &dp) != 0
        || read_u16(f, &rf) != 0 || read_u32(f, &vfc) != 0 || read_u32(f, &mva) != 0
        || read_u16(f, &pshare) != 0 || read_u8(f, &power) != 0
        || read_u8(f, &fee_mode) != 0) {
        return -1;
    }
    if (read_u32(f, &vacc) != 0 || read_u32(f, &vref) != 0
        || read_n(f, &iref, 4) != 0 || read_n(f, &last, 8) != 0) {
        return -1;
    }
    if (read_u64(f, &rx) != 0 || read_u64(f, &ry) != 0
        || read_n(f, &now, 8) != 0 || read_u16(f, &nbin) != 0) {
        return -1;
    }
    if (nbin > LIVE_BIN_MAX) {
        return -1;
    }
    s.active_id = active;
    s.bin_step = bin_step;
    s.status = status;
    s.parameters.base_factor = bf;
    s.parameters.filter_period = fp;
    s.parameters.decay_period = dp;
    s.parameters.reduction_factor = rf;
    s.parameters.variable_fee_control = vfc;
    s.parameters.max_volatility_accumulator = mva;
    s.parameters.protocol_share = pshare;
    s.parameters.base_fee_power_factor = power;
    s.parameters.collect_fee_mode = fee_mode;
    s.v_parameters.volatility_accumulator = vacc;
    s.v_parameters.volatility_reference = vref;
    s.v_parameters.index_reference = iref;
    s.v_parameters.last_update_timestamp = last;
    s.reserve_x = rx;
    s.reserve_y = ry;
    s.now_ts = now;
    s.nbin = nbin;
    for (i = 0; i < nbin; i++) {
        int32_t id;
        uint64_t x, y;
        if (read_n(f, &id, 4) != 0 || read_u64(f, &x) != 0 || read_u64(f, &y) != 0) {
            return -1;
        }
        s.bins[i].id = id;
        s.bins[i].amount_x = x;
        s.bins[i].amount_y = y;
    }
    if (dlmm_cache_put(&u->dlmm, idx, &s) != 0) {
        return -1;
    }
    u->n_dlmm++;
    return 0;
}

static int
load_pump(FILE *f, live_univ_t *u, uint32_t idx)
{
    pump_state_t *p = &u->pump[idx];

    memset(p, 0, sizeof(*p));
    if (read_u64(f, &p->reserve_base) != 0 || read_u64(f, &p->reserve_quote) != 0
        || read_n(f, &p->virtual_quote, 8) != 0 || read_u64(f, &p->lp_fee_bps) != 0
        || read_u64(f, &p->protocol_fee_bps) != 0
        || read_u64(f, &p->creator_fee_bps) != 0
        || read_u8(f, &p->disabled) != 0 || read_u8(f, &p->status) != 0) {
        return -1;
    }
    u->pump_live[idx] = 1;
    u->n_pump++;
    return 0;
}

static int
load_amm2(FILE *f, live_univ_t *u, uint32_t idx)
{
    amm2_state_t *a = &u->amm2[idx];

    memset(a, 0, sizeof(*a));
    if (read_u64(f, &a->reserve_x) != 0 || read_u64(f, &a->reserve_y) != 0
        || read_u16(f, &a->fee_bps) != 0 || read_u8(f, &a->status) != 0) {
        return -1;
    }
    u->extra_live[idx] = 1;
    if (u->meta[idx].protocol == PROTO_CPMM) {
        u->n_cpmm++;
    } else {
        u->n_damm++;
    }
    return 0;
}

static int
load_clmm(FILE *f, live_univ_t *u, uint32_t idx)
{
    clmm_state_t *c = &u->clmm[idx];

    memset(c, 0, sizeof(*c));
    if (read_n(f, &c->tick, 4) != 0 || read_u64(f, &c->sqrt_price_x64) != 0
        || read_u64(f, &c->liquidity) != 0 || read_u16(f, &c->fee_bps) != 0
        || read_u8(f, &c->status) != 0 || read_u8(f, &c->has_ticks) != 0) {
        return -1;
    }
    u->extra_live[idx] = 1;
    if (u->meta[idx].protocol == PROTO_CLMM) {
        u->n_clmm++;
    } else {
        u->n_orca++;
    }
    return 0;
}

int
live_univ_load(live_univ_t *u, const char *path)
{
    FILE *f;
    uint32_t magic;
    uint16_t ver;
    uint16_t n;
    uint32_t i;

    if (u == NULL || path == NULL) {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (read_u32(f, &magic) != 0 || magic != LIVE_MAGIC
        || read_u16(f, &ver) != 0 || (ver != LIVE_VER && ver != LIVE_VER2)
        || read_u16(f, &n) != 0 || n == 0
        || read_u64(f, &u->slot) != 0 || read_u64(f, &u->state_version) != 0) {
        fclose(f);
        return -1;
    }
    if (live_univ_reserve(u, n) != 0) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < n; i++) {
        live_meta_t *m = &u->meta[i];
        if (read_u8(f, &m->protocol) != 0
            || read_n(f, m->pubkey, 32) != 0
            || read_n(f, m->mint_x, 32) != 0
            || read_n(f, m->mint_y, 32) != 0
            || read_n(f, m->vault_x, 32) != 0
            || read_n(f, m->vault_y, 32) != 0) {
            fclose(f);
            return -1;
        }
        if (m->protocol == PROTO_DLMM) {
            if (load_dlmm(f, u, i) != 0) {
                fclose(f);
                return -1;
            }
        } else if (m->protocol == PROTO_PUMP) {
            if (load_pump(f, u, i) != 0) {
                fclose(f);
                return -1;
            }
        } else if (ver == LIVE_VER2
                   && (m->protocol == PROTO_CPMM || m->protocol == PROTO_DAMM)) {
            if (load_amm2(f, u, i) != 0) {
                fclose(f);
                return -1;
            }
        } else if (ver == LIVE_VER2
                   && (m->protocol == PROTO_CLMM || m->protocol == PROTO_ORCA)) {
            if (load_clmm(f, u, i) != 0) {
                fclose(f);
                return -1;
            }
        } else {
            fclose(f);
            return -1;
        }
        if (pool_table_put(&u->keys, m->pubkey, m->protocol) != 0) {
            fclose(f);
            return -1;
        }
        if (universe_add_pool(&u->routes, m->protocol, m->mint_x, m->mint_y) != 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    u->n = n;
    u->file_ver = ver;
    if (universe_compile(&u->routes,
                         ver == LIVE_VER2 ? UNIV_MASK_6 : UNIV_MASK_2) != 0) {
        return -1;
    }
    if (live_univ_assert_route0(u) != 0) {
        return -1;
    }
    return 0;
}

int
live_univ_assert_route0(const live_univ_t *u)
{
    uint32_t r;

    if (u == NULL) {
        return -1;
    }
    for (r = 0; r < u->routes.n_route; r++) {
        const compiled_route_t *cr = &u->routes.route[r];
        const live_meta_t *d = NULL;
        const live_meta_t *p = NULL;
        uint8_t h;
        if (cr->family != ROUTE_FAM_0_DLMM_PUMP) {
            continue;
        }
        for (h = 0; h < cr->n_hop; h++) {
            uint32_t i = cr->pool[h];
            if (i >= u->n) {
                return -1;
            }
            if (u->meta[i].protocol == PROTO_DLMM) {
                d = &u->meta[i];
            } else if (u->meta[i].protocol == PROTO_PUMP) {
                p = &u->meta[i];
            }
        }
        if (d == NULL || p == NULL) {
            fprintf(stderr, "ROUTE0_INCOMPATIBLE route=%u missing hop\n", r);
            return -1;
        }
        /* token != WSOL, dlmm.X == token, dlmm.Y == WSOL,
         * pump.base == token, pump.quote == WSOL */
        if (memcmp(d->mint_x, MINT_SOL, 32) == 0
            || memcmp(d->mint_y, MINT_SOL, 32) != 0
            || memcmp(p->mint_x, MINT_SOL, 32) == 0
            || memcmp(p->mint_y, MINT_SOL, 32) != 0
            || memcmp(d->mint_x, p->mint_x, 32) != 0) {
            fprintf(stderr,
                    "ROUTE0_INCOMPATIBLE route=%u refuse PAPER/LIVE\n", r);
            return -1;
        }
    }
    return 0;
}

int
live_dlmm_quote(const live_univ_t *u, uint32_t pool_idx,
                uint64_t amount_in, uint8_t swap_for_y,
                dlmm_quote_t *out)
{
    const dlmm_pool_hot_t *h;
    dlmm_state_t s;

    if (u == NULL || out == NULL || pool_idx >= u->n
        || u->meta[pool_idx].protocol != PROTO_DLMM) {
        return -1;
    }
    h = dlmm_cache_get(&u->dlmm, pool_idx);
    if (h == NULL) {
        return -1;
    }
    dlmm_hot_to_state(h, &s);
    return dlmm_quote_exact_in(&s, amount_in, swap_for_y, out);
}

int
live_pump_quote(const live_univ_t *u, uint32_t pool_idx,
                uint64_t amount_in, uint8_t direction,
                pump_quote_t *out)
{
    if (u == NULL || out == NULL || pool_idx >= u->n
        || u->meta[pool_idx].protocol != PROTO_PUMP
        || !u->pump_live[pool_idx]) {
        return -1;
    }
    return pump_quote_exact_in(&u->pump[pool_idx], amount_in, direction, out);
}

int
live_fill_univ_state(const live_univ_t *u, univ_state_t *st)
{
    uint32_t i;

    if (u == NULL || st == NULL || st->n < u->n) {
        return -1;
    }
    for (i = 0; i < u->n; i++) {
        st->amm2[i] = u->amm2[i];
        st->clmm[i] = u->clmm[i];
    }
    return 0;
}
