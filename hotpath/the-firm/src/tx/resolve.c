#include "tx/resolve.h"

#include "wire/lut_cache.h"

#include <string.h>

static int
eq32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32) == 0;
}

static const wire_lut_t *
lut_find(const wire_lut_tab_t *tab, const uint8_t key[32])
{
    uint32_t i;

    if (tab == NULL) {
        return NULL;
    }
    for (i = 0; i < tab->n; i++) {
        if (tab->t[i].used && eq32(tab->t[i].key, key)) {
            return &tab->t[i];
        }
    }
    return NULL;
}

static int
append_idx(tx_view_t *v, const wire_lut_t *lt, const uint8_t *idx, uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        uint8_t k = idx[i];

        if (k >= lt->n) {
            return TXV_RES_LUT;
        }
        if (v->n_keys >= TXV_KEY_MAX) {
            return TXV_RES_BAD;
        }
        memcpy(v->key[v->n_keys], lt->addr[k], 32);
        v->n_keys++;
    }
    return TXV_RES_OK;
}

int
txview_resolve(tx_view_t *v, const wire_lut_tab_t *luts)
{
    uint8_t i;
    const wire_lut_t *found[TXV_ALT_MAX];

    if (v == NULL) {
        return TXV_RES_BAD;
    }
    v->lut_missing = 0;
    v->n_keys = v->n_static;
    if (v->n_alt == 0) {
        return TXV_RES_OK;
    }
    for (i = 0; i < v->n_alt; i++) {
        found[i] = lut_find(luts, v->alt[i].table);
        if (found[i] == NULL) {
            v->lut_missing = 1;
            return TXV_RES_LUT;
        }
    }
    /* All writables, descriptor order, then all readables. */
    for (i = 0; i < v->n_alt; i++) {
        int rc = append_idx(v, found[i], v->alt[i].widx, v->alt[i].nw);

        if (rc != TXV_RES_OK) {
            v->lut_missing = 1;
            return rc;
        }
    }
    for (i = 0; i < v->n_alt; i++) {
        int rc = append_idx(v, found[i], v->alt[i].ridx, v->alt[i].nr);

        if (rc != TXV_RES_OK) {
            v->lut_missing = 1;
            return rc;
        }
    }
    return TXV_RES_OK;
}

static int
append_one(tx_view_t *v, lut_cache_t *c, const uint8_t table[32], uint8_t idx,
           uint64_t slot)
{
    if (v->n_keys >= TXV_KEY_MAX) {
        return TXV_RES_BAD;
    }
    if (lut_cache_lookup(c, table, idx, slot, v->key[v->n_keys]) != 0) {
        v->lut_missing = 1;
        c->n_lookup_fail++;
        return TXV_RES_LUT;
    }
    v->n_keys++;
    c->n_lookup_ok++;
    return TXV_RES_OK;
}

int
txview_resolve_at(tx_view_t *v, lut_cache_t *c, uint64_t slot)
{
    uint8_t i, j;
    int rc;

    if (v == NULL || c == NULL) {
        return TXV_RES_BAD;
    }
    v->lut_missing = 0;
    v->n_keys = v->n_static;
    if (v->n_alt == 0) {
        return TXV_RES_OK;
    }
    /* Do not unique descriptors: repeats are legal. */
    for (i = 0; i < v->n_alt; i++) {
        for (j = 0; j < v->alt[i].nw; j++) {
            rc = append_one(v, c, v->alt[i].table, v->alt[i].widx[j], slot);
            if (rc != TXV_RES_OK) {
                return rc;
            }
        }
    }
    for (i = 0; i < v->n_alt; i++) {
        for (j = 0; j < v->alt[i].nr; j++) {
            rc = append_one(v, c, v->alt[i].table, v->alt[i].ridx[j], slot);
            if (rc != TXV_RES_OK) {
                return rc;
            }
        }
    }
    return TXV_RES_OK;
}
