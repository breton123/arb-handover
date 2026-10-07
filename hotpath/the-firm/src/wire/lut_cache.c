#include "wire/lut_cache.h"

#include "wire/lut_file.h"

#include <string.h>

static int
eq32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32) == 0;
}

static int
is_quar(const lut_cache_t *c, const uint8_t table[32])
{
    uint32_t i;

    for (i = 0; i < c->n_quar; i++) {
        if (eq32(c->quar[i], table)) {
            return 1;
        }
    }
    return 0;
}

static void
quar_add(lut_cache_t *c, const uint8_t table[32])
{
    uint32_t i;

    if (is_quar(c, table)) {
        return;
    }
    if (c->n_quar >= LUT_QUAR_MAX) {
        return;
    }
    memcpy(c->quar[c->n_quar], table, 32);
    c->n_quar++;
    for (i = 0; i < c->n_sp; i++) {
        if (eq32(c->sp[i].table, table)) {
            c->sp[i].quar = 1;
            c->sp[i].live = 0;
        }
    }
}

static lut_account_t
meta_from_ent(const lut_sparse_ent_t *e)
{
    lut_account_t a;

    memset(&a, 0, sizeof(a));
    a.have_meta = e->have_meta;
    a.deactivation_slot = e->deactivation_slot;
    a.last_extended_slot = e->last_extended_slot;
    a.start_index = e->start_index;
    return a;
}

void
lut_cache_clear(lut_cache_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->full = &c->owned;
    wire_lut_tab_clear(&c->owned);
}

void
lut_cache_use_full(lut_cache_t *c, wire_lut_tab_t *tab)
{
    if (c == NULL) {
        return;
    }
    c->full = (tab != NULL) ? tab : &c->owned;
}

int
lut_cache_lookup(const lut_cache_t *c, const uint8_t table[32], uint8_t idx,
                 uint64_t slot, uint8_t out[32])
{
    uint32_t i;
    const wire_lut_tab_t *tab;

    if (c == NULL || table == NULL || out == NULL) {
        return -1;
    }
    if (is_quar(c, table)) {
        return -1;
    }
    tab = c->full;
    if (tab != NULL) {
        for (i = 0; i < tab->n; i++) {
            if (!tab->t[i].used || !eq32(tab->t[i].key, table)) {
                continue;
            }
            if (idx >= tab->t[i].n) {
                continue;
            }
            if (tab == &c->owned && lut_index_blocked(&c->meta[i], slot, idx)) {
                return -1;
            }
            memcpy(out, tab->t[i].addr[idx], 32);
            return 0;
        }
    }
    for (i = 0; i < c->n_sp; i++) {
        lut_account_t tmp;

        if (!c->sp[i].live || c->sp[i].quar || c->sp[i].idx != idx
            || !eq32(c->sp[i].table, table)) {
            continue;
        }
        tmp = meta_from_ent(&c->sp[i]);
        if (lut_index_blocked(&tmp, slot, idx)) {
            return -1;
        }
        memcpy(out, c->sp[i].addr, 32);
        return 0;
    }
    return -1;
}

int
lut_cache_learn_sparse(lut_cache_t *c, const uint8_t table[32], uint8_t idx,
                       const uint8_t addr[32], const lut_account_t *meta)
{
    uint8_t have[32];
    uint32_t i;

    if (c == NULL || table == NULL || addr == NULL || c->sealed) {
        return -1;
    }
    if (lut_cache_lookup(c, table, idx, LUT_SLOT_NEVER, have) == 0) {
        if (memcmp(have, addr, 32) != 0) {
            quar_add(c, table);
            return -1;
        }
        return 0;
    }
    if (c->n_sp >= LUT_SPARSE_MAX) {
        return -1;
    }
    i = c->n_sp++;
    memset(&c->sp[i], 0, sizeof(c->sp[i]));
    memcpy(c->sp[i].table, table, 32);
    memcpy(c->sp[i].addr, addr, 32);
    c->sp[i].idx = idx;
    c->sp[i].live = 1;
    if (meta != NULL && meta->have_meta) {
        c->sp[i].have_meta = 1;
        c->sp[i].start_index = meta->start_index;
        c->sp[i].last_extended_slot = meta->last_extended_slot;
        c->sp[i].deactivation_slot = meta->deactivation_slot;
    }
    c->n_learn++;
    c->generation++;
    return 0;
}

int
lut_cache_install_account(lut_cache_t *c, const uint8_t table[32],
                          const lut_account_t *acc)
{
    uint32_t i, n;
    uint8_t have[32];

    if (c == NULL || table == NULL || acc == NULL || c->full == NULL) {
        return -1;
    }
    n = acc->n;
    for (i = 0; i < n; i++) {
        if (lut_cache_lookup(c, table, (uint8_t)i, LUT_SLOT_NEVER, have) == 0
            && memcmp(have, acc->addr[i], 32) != 0) {
            quar_add(c, table);
            return -1;
        }
    }
    if (wire_lut_put(c->full, table, acc->addr[0], n) != 0) {
        for (i = 0; i < n; i++) {
            if (lut_cache_learn_sparse(c, table, (uint8_t)i, acc->addr[i],
                                       acc) != 0) {
                return -1;
            }
        }
        c->n_install++;
        c->generation++;
        return 0;
    }
    if (c->full == &c->owned) {
        for (i = 0; i < c->owned.n; i++) {
            if (c->owned.t[i].used && eq32(c->owned.t[i].key, table)) {
                c->meta[i] = *acc;
                break;
            }
        }
    }
    c->n_install++;
    c->generation++;
    return 0;
}

static void
miss_add(lut_cache_t *c, const uint8_t table[32])
{
    uint32_t i;

    for (i = 0; i < c->n_miss; i++) {
        if (eq32(c->miss[i], table)) {
            return;
        }
    }
    if (c->n_miss >= LUT_MISS_MAX) {
        return;
    }
    memcpy(c->miss[c->n_miss], table, 32);
    c->n_miss++;
}

void
lut_cache_note_view(lut_cache_t *c, const tx_view_t *v, uint64_t slot)
{
    uint8_t i, j, dummy[32];

    if (c == NULL || v == NULL || c->sealed) {
        return;
    }
    for (i = 0; i < v->n_alt; i++) {
        const tx_alt_t *a = &v->alt[i];

        for (j = 0; j < a->nw; j++) {
            if (lut_cache_lookup(c, a->table, a->widx[j], slot, dummy) != 0) {
                miss_add(c, a->table);
            }
        }
        for (j = 0; j < a->nr; j++) {
            if (lut_cache_lookup(c, a->table, a->ridx[j], slot, dummy) != 0) {
                miss_add(c, a->table);
            }
        }
    }
}

static int
table_in_full(const lut_cache_t *c, const uint8_t table[32])
{
    uint32_t i;
    const wire_lut_tab_t *tab;

    tab = c->full;
    if (tab == NULL) {
        return 0;
    }
    for (i = 0; i < tab->n; i++) {
        if (tab->t[i].used && eq32(tab->t[i].key, table)) {
            return 1;
        }
    }
    return 0;
}

static int
table_has_sparse(const lut_cache_t *c, const uint8_t table[32])
{
    uint32_t i;

    for (i = 0; i < c->n_sp; i++) {
        if (c->sp[i].live && eq32(c->sp[i].table, table)) {
            return 1;
        }
    }
    return 0;
}

static int
load_install(void *ctx, const uint8_t key[32], const uint8_t *addrs,
             uint32_t n)
{
    lut_cache_t *c = ctx;
    lut_account_t acc;
    uint32_t i;
    uint8_t dummy[32];

    if (n > 0
        && lut_cache_lookup(c, key, 0, LUT_SLOT_NEVER, dummy) == 0
        && lut_cache_lookup(c, key, (uint8_t)(n - 1u), LUT_SLOT_NEVER, dummy)
               == 0) {
        return 0;
    }
    memset(&acc, 0, sizeof(acc));
    acc.n = (uint16_t)n;
    acc.have_meta = 0;
    for (i = 0; i < n && i < LUT_ADDR_MAX; i++) {
        memcpy(acc.addr[i], addrs + i * 32u, 32);
    }
    (void)lut_cache_install_account(c, key, &acc);
    return 0;
}

int
lut_cache_load_jsonl(lut_cache_t *c, const char *path)
{
    if (c == NULL) {
        return -1;
    }
    if (c->full == NULL) {
        c->full = &c->owned;
    }
    return wire_lut_jsonl_each(path, load_install, c);
}

void
lut_cache_prune_miss(lut_cache_t *c)
{
    uint32_t i, w;

    if (c == NULL) {
        return;
    }
    w = 0;
    for (i = 0; i < c->n_miss; i++) {
        if (table_in_full(c, c->miss[i]) || table_has_sparse(c, c->miss[i])) {
            continue;
        }
        if (w != i) {
            memcpy(c->miss[w], c->miss[i], 32);
        }
        w++;
    }
    c->n_miss = w;
    c->n_miss_written = w;
}

int
lut_cache_flush_miss(lut_cache_t *c, FILE *fp)
{
    uint32_t i;

    if (c == NULL || fp == NULL) {
        return -1;
    }
    for (i = c->n_miss_written; i < c->n_miss; i++) {
        uint32_t b;

        fputs("{\"table_hex\":\"", fp);
        for (b = 0; b < 32u; b++) {
            fprintf(fp, "%02x", c->miss[i][b]);
        }
        fputs("\"}\n", fp);
    }
    c->n_miss_written = c->n_miss;
    fflush(fp);
    return 0;
}
