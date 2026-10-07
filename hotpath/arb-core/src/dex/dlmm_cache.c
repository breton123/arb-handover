#include "dlmm_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t
scalar_bytes(void)
{
    return (uint32_t)(sizeof(uint8_t) + sizeof(uint16_t)
        + sizeof(int32_t)
        + sizeof(dlmm_static_params_t)
        + sizeof(dlmm_vparams_t)
        + 2u * sizeof(uint64_t)
        + sizeof(int64_t));
}

void
dlmm_cache_init(dlmm_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
}

int
dlmm_cache_reserve(dlmm_cache_t *c, uint32_t cap)
{
    dlmm_pool_hot_t *p;

    if (c == NULL || cap == 0 || cap > DLMM_CACHE_MAX_POOLS) {
        return -1;
    }
    if (c->cap >= cap && c->pool != NULL) {
        return 0;
    }
    p = realloc(c->pool, (size_t)cap * sizeof(*p));
    if (p == NULL) {
        return -1;
    }
    if (cap > c->cap) {
        memset(p + c->cap, 0, (size_t)(cap - c->cap) * sizeof(*p));
    }
    c->pool = p;
    c->cap = cap;
    return 0;
}

void
dlmm_cache_free(dlmm_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    free(c->pool);
    memset(c, 0, sizeof(*c));
}

int
dlmm_hot_slot(const dlmm_pool_hot_t *h, int32_t id)
{
    int64_t lo;
    int64_t off;

    if (h == NULL) {
        return -1;
    }
    lo = (int64_t)h->window_center - (int64_t)DLMM_CACHE_K;
    off = (int64_t)id - lo;
    if (off < 0 || off >= (int64_t)DLMM_CACHE_WINDOW) {
        return -1;
    }
    return (int)off;
}

void
dlmm_hot_to_state(const dlmm_pool_hot_t *h, dlmm_state_t *s)
{
    uint16_t i;

    memset(s, 0, sizeof(*s));
    s->active_id = h->active_id;
    s->bin_step = h->bin_step;
    s->status = h->status;
    s->parameters = h->parameters;
    s->v_parameters = h->v_parameters;
    s->reserve_x = h->reserve_x;
    s->reserve_y = h->reserve_y;
    s->now_ts = h->now_ts;
    s->nbin = 0;
    for (i = 0; i < DLMM_CACHE_WINDOW && s->nbin < DLMM_BINS_MAX; i++) {
        if (h->live[i]) {
            s->bins[s->nbin].id = h->bin[i].id;
            s->bins[s->nbin].amount_x = h->bin[i].amount_x;
            s->bins[s->nbin].amount_y = h->bin[i].amount_y;
            s->bins[s->nbin].price = 0;
            s->nbin++;
        }
    }
}

int
dlmm_state_into_hot(const dlmm_state_t *s, dlmm_pool_hot_t *h)
{
    uint16_t i;

    if (s == NULL || h == NULL) {
        return -1;
    }
    h->active_id = s->active_id;
    h->bin_step = s->bin_step;
    h->status = s->status;
    h->parameters = s->parameters;
    h->v_parameters = s->v_parameters;
    h->reserve_x = s->reserve_x;
    h->reserve_y = s->reserve_y;
    h->now_ts = s->now_ts;
    for (i = 0; i < s->nbin; i++) {
        int slot = dlmm_hot_slot(h, s->bins[i].id);
        if (slot < 0) {
            continue;
        }
        h->bin[slot].id = s->bins[i].id;
        h->bin[slot].amount_x = s->bins[i].amount_x;
        h->bin[slot].amount_y = s->bins[i].amount_y;
        h->live[slot] = 1;
    }
    return 0;
}

int
dlmm_cache_put(dlmm_cache_t *c, uint32_t pool_idx, const dlmm_state_t *s)
{
    dlmm_pool_hot_t *h;
    uint16_t i;

    if (c == NULL || s == NULL || pool_idx >= DLMM_CACHE_MAX_POOLS) {
        return -1;
    }
    if (pool_idx >= c->cap) {
        uint32_t nc = c->cap != 0 ? c->cap : 8u;
        while (nc <= pool_idx) {
            if (nc > DLMM_CACHE_MAX_POOLS / 2u) {
                nc = DLMM_CACHE_MAX_POOLS;
                break;
            }
            nc *= 2u;
        }
        if (dlmm_cache_reserve(c, nc) != 0) {
            return -1;
        }
    }
    h = &c->pool[pool_idx];
    memset(h, 0, sizeof(*h));
    h->occupied = 1;
    h->window_center = s->active_id;
    if (dlmm_state_into_hot(s, h) != 0) {
        return -1;
    }
    for (i = 0; i < DLMM_CACHE_WINDOW; i++) {
        if (!h->live[i]) {
            h->bin[i].id = h->window_center - (int32_t)DLMM_CACHE_K + (int32_t)i;
        }
    }
    if (pool_idx + 1u > c->n) {
        c->n = pool_idx + 1u;
    }
    return 0;
}

const dlmm_pool_hot_t *
dlmm_cache_get(const dlmm_cache_t *c, uint32_t pool_idx)
{
    if (c == NULL || c->pool == NULL || pool_idx >= c->cap
        || !c->pool[pool_idx].occupied) {
        return NULL;
    }
    return &c->pool[pool_idx];
}

int
dlmm_cache_predict(const dlmm_cache_t *c, uint32_t pool_idx,
                   const dlmm_swap_ix_t *ix,
                   dlmm_pool_hot_t *spec,
                   dlmm_apply_result_t *res)
{
    const dlmm_pool_hot_t *src;
    dlmm_state_t before;
    dlmm_state_t after;

    src = dlmm_cache_get(c, pool_idx);
    if (src == NULL || spec == NULL || ix == NULL || res == NULL) {
        return -1;
    }
    *spec = *src;
    dlmm_hot_to_state(spec, &before);
    if (dlmm_apply_swap(&before, ix, &after, res) != 0) {
        return -1;
    }
    if (dlmm_state_into_hot(&after, spec) != 0) {
        return -1;
    }
    return 0;
}

int
dlmm_cache_commit(dlmm_cache_t *c, uint32_t pool_idx, const dlmm_pool_hot_t *spec)
{
    if (c == NULL || spec == NULL || c->pool == NULL || pool_idx >= c->cap) {
        return -1;
    }
    if (!c->pool[pool_idx].occupied || !spec->occupied) {
        return -1;
    }
    c->pool[pool_idx] = *spec;
    if (pool_idx + 1u > c->n) {
        c->n = pool_idx + 1u;
    }
    return 0;
}

int
dlmm_hot_econ_eq(const dlmm_pool_hot_t *a, const dlmm_pool_hot_t *b)
{
    uint16_t i;

    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->active_id != b->active_id
        || a->bin_step != b->bin_step
        || a->status != b->status
        || a->reserve_x != b->reserve_x
        || a->reserve_y != b->reserve_y
        || a->v_parameters.volatility_accumulator != b->v_parameters.volatility_accumulator
        || a->v_parameters.volatility_reference != b->v_parameters.volatility_reference
        || a->v_parameters.index_reference != b->v_parameters.index_reference) {
        return 0;
    }
    for (i = 0; i < DLMM_CACHE_WINDOW; i++) {
        if (a->live[i] != b->live[i]) {
            return 0;
        }
        if (!a->live[i]) {
            continue;
        }
        if (a->bin[i].id != b->bin[i].id
            || a->bin[i].amount_x != b->bin[i].amount_x
            || a->bin[i].amount_y != b->bin[i].amount_y) {
            return 0;
        }
    }
    return 1;
}

uint32_t
dlmm_hot_bytes(const dlmm_pool_hot_t *h)
{
    uint16_t i;
    uint32_t n = 0;

    if (h == NULL) {
        return 0;
    }
    for (i = 0; i < DLMM_CACHE_WINDOW; i++) {
        if (h->live[i]) {
            n++;
        }
    }
    return scalar_bytes() + n * (uint32_t)sizeof(dlmm_hot_bin_t);
}

static void
state_from_cap_bins(const dlmm_cap_rec_t *rec, int before, dlmm_state_t *s)
{
    const dlmm_hot_bin_t *bins = before ? rec->bins_before : rec->bins_after;
    uint16_t n = before ? rec->nbin_before : rec->nbin_after;
    uint16_t i;

    memset(s, 0, sizeof(*s));
    s->active_id = before ? rec->before_active : rec->after_active;
    s->bin_step = rec->bin_step;
    s->status = rec->status;
    s->parameters = rec->parameters;
    s->v_parameters = before ? rec->v_before : rec->v_after;
    s->reserve_x = before ? rec->before_rx : rec->after_rx;
    s->reserve_y = before ? rec->before_ry : rec->after_ry;
    s->now_ts = rec->now_ts;
    if (n > DLMM_BINS_MAX) {
        n = DLMM_BINS_MAX;
    }
    s->nbin = n;
    for (i = 0; i < n; i++) {
        s->bins[i].id = bins[i].id;
        s->bins[i].amount_x = bins[i].amount_x;
        s->bins[i].amount_y = bins[i].amount_y;
    }
}

int
dlmm_cap_save(const char *path, const dlmm_cap_rec_t *recs, uint32_t n)
{
    FILE *f;
    dlmm_cap_hdr_t hdr;
    size_t wr;

    if (path == NULL || (recs == NULL && n > 0)) {
        return -1;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = DLMM_CAP_MAGIC;
    hdr.ver = DLMM_CAP_VER;
    hdr.rec_bytes = (uint16_t)sizeof(dlmm_cap_rec_t);
    hdr.nrec = n;
    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }
    if (n > 0) {
        wr = fwrite(recs, sizeof(recs[0]), n, f);
        if (wr != (size_t)n) {
            fclose(f);
            return -1;
        }
    }
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;
}

int
dlmm_cap_load(const char *path, dlmm_cap_rec_t **recs, uint32_t *n)
{
    FILE *f;
    dlmm_cap_hdr_t hdr;
    dlmm_cap_rec_t *buf;
    size_t rd;

    if (path == NULL || recs == NULL || n == NULL) {
        return -1;
    }
    *recs = NULL;
    *n = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fread(&hdr, sizeof(hdr), 1, f) != 1
        || hdr.magic != DLMM_CAP_MAGIC
        || hdr.ver != DLMM_CAP_VER
        || hdr.rec_bytes != (uint16_t)sizeof(dlmm_cap_rec_t)) {
        fclose(f);
        return -1;
    }
    if (hdr.nrec == 0) {
        fclose(f);
        return 0;
    }
    buf = calloc(hdr.nrec, sizeof(*buf));
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    rd = fread(buf, sizeof(*buf), hdr.nrec, f);
    fclose(f);
    if (rd != (size_t)hdr.nrec) {
        free(buf);
        return -1;
    }
    *recs = buf;
    *n = hdr.nrec;
    return 0;
}

void
dlmm_cap_free(dlmm_cap_rec_t *recs)
{
    free(recs);
}

int
dlmm_cap_check(const dlmm_cap_rec_t *rec, dlmm_cap_verdict_t *v)
{
    dlmm_cache_t cache;
    dlmm_state_t before;
    dlmm_state_t expect;
    dlmm_pool_hot_t spec;
    dlmm_swap_ix_t ix;
    dlmm_apply_result_t res;
    int32_t delta;

    if (rec == NULL || v == NULL) {
        return -1;
    }
    memset(v, 0, sizeof(*v));
    state_from_cap_bins(rec, 1, &before);
    state_from_cap_bins(rec, 0, &expect);
    dlmm_cache_init(&cache);
    if (dlmm_cache_put(&cache, rec->pool_idx, &before) != 0) {
        dlmm_cache_free(&cache);
        return -1;
    }
    v->hot_bytes = dlmm_hot_bytes(dlmm_cache_get(&cache, rec->pool_idx));
    delta = rec->after_active - rec->before_active;
    if (delta < 0) {
        delta = -delta;
    }
    v->crossed = (uint16_t)delta;
    ix.amount_in = rec->amount_in;
    ix.min_amount_out = rec->min_out;
    ix.swap_for_y = rec->swap_for_y;
    if (dlmm_cache_predict(&cache, rec->pool_idx, &ix, &spec, &res) != 0) {
        v->insufficient = 1;
        v->match = 0;
        dlmm_cache_free(&cache);
        return 0;
    }
    {
        uint16_t i;
        int ok = 1;

        if (spec.active_id != expect.active_id
            || spec.reserve_x != expect.reserve_x
            || spec.reserve_y != expect.reserve_y
            || spec.v_parameters.volatility_accumulator
                != expect.v_parameters.volatility_accumulator) {
            ok = 0;
        }
        for (i = 0; ok && i < expect.nbin; i++) {
            int slot = dlmm_hot_slot(&spec, expect.bins[i].id);
            if (slot < 0 || !spec.live[slot]
                || spec.bin[slot].amount_x != expect.bins[i].amount_x
                || spec.bin[slot].amount_y != expect.bins[i].amount_y) {
                ok = 0;
            }
        }
        v->match = (uint8_t)ok;
    }
    v->crossed = (uint16_t)((spec.active_id > before.active_id)
        ? (spec.active_id - before.active_id)
        : (before.active_id - spec.active_id));
    dlmm_cache_free(&cache);
    return 0;
}
