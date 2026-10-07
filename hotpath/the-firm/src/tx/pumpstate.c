#include "tx/pumpstate.h"

#include "ingress/resolve.h"
#include "tx/bind.h"
#include "tx/resolve.h"

#include <string.h>

static int
is_pump_fast(uint8_t kind)
{
    return kind == IX_KIND_PUMP_SELL || kind == IX_KIND_PUMP_BUY_EQ
        || kind == IX_KIND_PUMP_BUY;
}

const char *
pumpstate_name(int rc)
{
    switch (rc) {
    case PS_OK:
        return "OK";
    case PS_DECODE:
        return "DECODE";
    case PS_LUT:
        return "LUT";
    case PS_UNSUPPORTED:
        return "UNSUPPORTED";
    case PS_NO_PRESTATE:
        return "NO_PRESTATE";
    case PS_APPLY:
        return "APPLY";
    case PS_IRRELEVANT:
        return "IRRELEVANT";
    case PS_GAP:
        return "GAP";
    case PS_BEFORE_ANCHOR:
        return "BEFORE_ANCHOR";
    default:
        return "OTHER";
    }
}

static void
dirty_pump_pools(compact_state_t *st, const ordered_tx_t *tx)
{
    uint16_t i;

    for (i = 0; i < tx->n_ix; i++) {
        if (tx->ix[i].proto == PROTO_PUMP
            && tx->ix[i].pool_id != STATE_ACCT_NONE) {
            (void)compact_pool_mark_dirty(st, tx->ix[i].pool_id);
        }
    }
}

int
pumpstate_from_bytes(compact_state_t *st, const wire_lut_tab_t *luts,
                     const uint8_t *bytes, uint32_t len, uint64_t slot,
                     pumpstate_out_t *out)
{
    uint16_t i;
    int rc, n_pump = 0, n_fast = 0;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    ordered_tx_clear(&out->tx);
    if (txview_decode(bytes, len, &out->view) != TXV_OK) {
        out->rc = PS_DECODE;
        return PS_DECODE;
    }
    rc = txview_resolve(&out->view, luts);
    out->lut_resolved = (uint8_t)(out->view.n_alt == 0 || rc == TXV_RES_OK);
    if (rc == TXV_RES_LUT) {
        out->rc = PS_LUT;
        return PS_LUT;
    }
    if (rc != TXV_RES_OK || txview_bind(&out->view, slot, &out->tx) != 0) {
        out->rc = PS_DECODE;
        return PS_DECODE;
    }
    if (st != NULL) {
        (void)ingress_resolve(st, &out->tx);
    }
    for (i = 0; i < out->tx.n_ix; i++) {
        if (out->tx.ix[i].proto != PROTO_PUMP) {
            continue;
        }
        n_pump++;
    }
    out->n_pump = (uint8_t)n_pump;
    if (n_pump == 0) {
        out->rc = PS_IRRELEVANT;
        return PS_IRRELEVANT;
    }
    if (st == NULL) {
        out->rc = PS_NO_PRESTATE;
        return PS_NO_PRESTATE;
    }
    n_fast = 0;
    for (i = 0; i < out->tx.n_ix; i++) {
        ordered_ix_t *ix = &out->tx.ix[i];
        const state_pool_t *p;

        if (ix->proto != PROTO_PUMP) {
            continue;
        }
        if (ix->pool_id == STATE_ACCT_NONE
            || compact_pool_get(st, ix->pool_id, &p) != 0 || !p->live) {
            if (!is_pump_fast(ix->kind)) {
                out->rc = PS_UNSUPPORTED;
                return PS_UNSUPPORTED;
            }
            out->rc = PS_NO_PRESTATE;
            return PS_NO_PRESTATE;
        }
        if (p->anchor_slot != 0 && slot <= p->anchor_slot) {
            ix->relevant = 0;
            continue;
        }
        if (!is_pump_fast(ix->kind)) {
            dirty_pump_pools(st, &out->tx);
            out->rc = PS_UNSUPPORTED;
            return PS_UNSUPPORTED;
        }
        if (p->auth_bits != POOL_AUTH_READY
            || p->grade != POOL_GRADE_EXACT) {
            out->rc = PS_GAP;
            return PS_GAP;
        }
        n_fast++;
    }
    if (n_fast == 0) {
        out->rc = PS_BEFORE_ANCHOR;
        return PS_BEFORE_ANCHOR;
    }
    if (apply_tx(st, &out->tx, &out->ar) != APPLY_OK) {
        dirty_pump_pools(st, &out->tx);
        out->rc = PS_APPLY;
        return PS_APPLY;
    }
    out->predicted = 1;
    out->rc = PS_OK;
    return PS_OK;
}

static void
dirty_static_pools(compact_state_t *st, const tx_view_t *v)
{
    uint16_t i;
    uint32_t id;

    if (st == NULL || v == NULL) {
        return;
    }
    for (i = 0; i < v->n_static; i++) {
        if (compact_pool_find(st, v->key[i], &id) == 0) {
            (void)compact_pool_mark_dirty(st, id);
        }
    }
}

int
pumpstate_from_cache(compact_state_t *st, lut_cache_t *luts,
                     const uint8_t *bytes, uint32_t len, uint64_t slot,
                     pumpstate_out_t *out)
{
    uint16_t i;
    int rc, n_pump = 0, n_fast = 0;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    ordered_tx_clear(&out->tx);
    if (txview_decode(bytes, len, &out->view) != TXV_OK) {
        out->rc = PS_DECODE;
        return PS_DECODE;
    }
    rc = txview_resolve_at(&out->view, luts, slot);
    out->lut_resolved = (uint8_t)(out->view.n_alt == 0 || rc == TXV_RES_OK);
    if (rc == TXV_RES_LUT) {
        if (luts != NULL) {
            lut_cache_note_view(luts, &out->view, slot);
        }
        dirty_static_pools(st, &out->view);
        out->rc = PS_LUT;
        return PS_LUT;
    }
    if (rc != TXV_RES_OK || txview_bind(&out->view, slot, &out->tx) != 0) {
        out->rc = PS_DECODE;
        return PS_DECODE;
    }
    if (st != NULL) {
        (void)ingress_resolve(st, &out->tx);
    }
    for (i = 0; i < out->tx.n_ix; i++) {
        if (out->tx.ix[i].proto != PROTO_PUMP) {
            continue;
        }
        n_pump++;
    }
    out->n_pump = (uint8_t)n_pump;
    if (n_pump == 0) {
        out->rc = PS_IRRELEVANT;
        return PS_IRRELEVANT;
    }
    if (st == NULL) {
        out->rc = PS_NO_PRESTATE;
        return PS_NO_PRESTATE;
    }
    n_fast = 0;
    for (i = 0; i < out->tx.n_ix; i++) {
        ordered_ix_t *ix = &out->tx.ix[i];
        const state_pool_t *p;

        if (ix->proto != PROTO_PUMP) {
            continue;
        }
        if (ix->pool_id == STATE_ACCT_NONE
            || compact_pool_get(st, ix->pool_id, &p) != 0 || !p->live) {
            if (!is_pump_fast(ix->kind)) {
                out->rc = PS_UNSUPPORTED;
                return PS_UNSUPPORTED;
            }
            out->rc = PS_NO_PRESTATE;
            return PS_NO_PRESTATE;
        }
        if (p->anchor_slot != 0 && slot <= p->anchor_slot) {
            ix->relevant = 0;
            continue;
        }
        if (!is_pump_fast(ix->kind)) {
            dirty_pump_pools(st, &out->tx);
            out->rc = PS_UNSUPPORTED;
            return PS_UNSUPPORTED;
        }
        if (p->auth_bits != POOL_AUTH_READY
            || p->grade != POOL_GRADE_EXACT) {
            out->rc = PS_GAP;
            return PS_GAP;
        }
        n_fast++;
    }
    if (n_fast == 0) {
        out->rc = PS_BEFORE_ANCHOR;
        return PS_BEFORE_ANCHOR;
    }
    if (apply_tx(st, &out->tx, &out->ar) != APPLY_OK) {
        dirty_pump_pools(st, &out->tx);
        out->rc = PS_APPLY;
        return PS_APPLY;
    }
    out->predicted = 1;
    out->rc = PS_OK;
    return PS_OK;
}
