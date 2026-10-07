#include "transitions/overlay.h"

#include <string.h>

void
overlay_clear(tx_overlay_t *ov)
{
    if (ov == NULL) {
        return;
    }
    memset(ov, 0, sizeof(*ov));
}

static overlay_row_t *
find_row(tx_overlay_t *ov, uint32_t pool_id)
{
    uint8_t i;

    for (i = 0; i < ov->n; i++) {
        if (ov->row[i].pool_id == pool_id) {
            return &ov->row[i];
        }
    }
    return NULL;
}

static int
ensure_row(tx_overlay_t *ov, const compact_state_t *st, uint32_t pool_id,
           overlay_row_t **out)
{
    overlay_row_t *r;
    const state_pool_t *p;

    r = find_row(ov, pool_id);
    if (r != NULL) {
        *out = r;
        return 0;
    }
    if (ov->n >= STATE_OVERLAY_MAX) {
        return -1;
    }
    if (compact_pool_get(st, pool_id, &p) != 0 || !p->live
        || p->proto != STATE_PROTO_PUMP) {
        return -1;
    }
    r = &ov->row[ov->n];
    memset(r, 0, sizeof(*r));
    r->pool_id = pool_id;
    r->incarnation_base = p->incarnation;
    r->pump = p->pump;
    ov->n++;
    *out = r;
    return 0;
}

int
overlay_ensure_token(tx_overlay_t *ov, const compact_state_t *st,
                     uint32_t id, uint8_t allow_dead, overlay_token_row_t **out)
{
    uint8_t i;
    const state_token_t *t;
    overlay_token_row_t *r;

    if (ov == NULL || out == NULL || id == STATE_ACCT_NONE) {
        return -1;
    }
    for (i = 0; i < ov->n_token; i++) {
        if (ov->token[i].id == id) {
            *out = &ov->token[i];
            return 0;
        }
    }
    if (ov->n_token >= STATE_OVERLAY_TOKEN_MAX) {
        return -1;
    }
    if (compact_token_get(st, id, &t) != 0) {
        return -1;
    }
    if (!t->live && !allow_dead) {
        return -1;
    }
    if (t->extensions_mask != 0) {
        return -1;
    }
    r = &ov->token[ov->n_token];
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->incarnation_base = t->incarnation;
    r->tok = *t;
    ov->n_token++;
    *out = r;
    return 0;
}

int
overlay_ensure_sys(tx_overlay_t *ov, const compact_state_t *st, uint32_t id,
                   overlay_sys_row_t **out)
{
    uint8_t i;
    const state_sys_t *s;
    overlay_sys_row_t *r;

    if (ov == NULL || out == NULL || id == STATE_ACCT_NONE) {
        return -1;
    }
    for (i = 0; i < ov->n_sys; i++) {
        if (ov->sys[i].id == id) {
            *out = &ov->sys[i];
            return 0;
        }
    }
    if (ov->n_sys >= STATE_OVERLAY_SYS_MAX) {
        return -1;
    }
    if (compact_sys_get(st, id, &s) != 0 || !s->live) {
        return -1;
    }
    r = &ov->sys[ov->n_sys];
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->incarnation_base = s->incarnation;
    r->sys = *s;
    ov->n_sys++;
    *out = r;
    return 0;
}

static int
clone_token_id(tx_overlay_t *ov, const compact_state_t *st, uint32_t id,
               uint8_t allow_dead)
{
    overlay_token_row_t *r;

    if (id == STATE_ACCT_NONE) {
        return 0;
    }
    return overlay_ensure_token(ov, st, id, allow_dead, &r);
}

static int
clone_sys_id(tx_overlay_t *ov, const compact_state_t *st, uint32_t id)
{
    overlay_sys_row_t *r;

    if (id == STATE_ACCT_NONE) {
        return 0;
    }
    return overlay_ensure_sys(ov, st, id, &r);
}

int
overlay_begin(tx_overlay_t *ov, const compact_state_t *st,
              const ordered_tx_t *tx)
{
    uint16_t i;

    if (ov == NULL) {
        return -1;
    }
    overlay_clear(ov);
    if (st == NULL || tx == NULL) {
        ov->status = OV_FAIL;
        return -1;
    }
    for (i = 0; i < tx->n_ix && i < STATE_IX_MAX; i++) {
        overlay_row_t *r;
        const ordered_ix_t *ix = &tx->ix[i];
        uint8_t allow_dead;

        if (!ix->relevant) {
            continue;
        }
        allow_dead = (uint8_t)(ix->kind == IX_KIND_ATA_CREATE
                               || ix->kind == IX_KIND_ATA_INIT);
        if (ix->pool_id != STATE_ACCT_NONE) {
            if (ensure_row(ov, st, ix->pool_id, &r) != 0) {
                ov->status = OV_FAIL;
                return -1;
            }
        }
        if (clone_token_id(ov, st, ix->src_token, 0) != 0
            || clone_token_id(ov, st, ix->dst_token, allow_dead) != 0
            || clone_token_id(ov, st, ix->vault_base, 0) != 0
            || clone_token_id(ov, st, ix->vault_quote, 0) != 0
            || clone_token_id(ov, st, ix->fee_proto, 0) != 0
            || clone_token_id(ov, st, ix->fee_creator, 0) != 0
            || clone_sys_id(ov, st, ix->src_sys) != 0
            || clone_sys_id(ov, st, ix->dst_sys) != 0) {
            ov->status = OV_FAIL;
            return -1;
        }
    }
    return 0;
}

int
overlay_apply_pump_ix(tx_overlay_t *ov, const compact_state_t *st,
                      const ordered_ix_t *ix, pump_swap_result_t *res)
{
    overlay_row_t *r;
    pump_swap_ix_t pix;
    pump_state_t after;

    if (ov == NULL || ix == NULL) {
        return -1;
    }
    if (ensure_row(ov, st, ix->pool_id, &r) != 0) {
        ov->status = OV_FAIL;
        return -1;
    }
    if (ix->kind == IX_KIND_PUMP_BUY) {
        if (pump_apply_buy_exact_out(&r->pump, ix->amount_in, ix->min_out,
                                     &after, res) != 0) {
            ov->status = OV_FAIL;
            return -1;
        }
    } else if (ix->kind == IX_KIND_PUMP_SELL
               || ix->kind == IX_KIND_PUMP_BUY_EQ) {
        memset(&pix, 0, sizeof(pix));
        pix.amount_in = ix->amount_in;
        pix.min_amount_out = ix->min_out;
        pix.direction = ix->direction;
        if (pump_apply_swap(&r->pump, &pix, &after, res) != 0) {
            ov->status = OV_FAIL;
            return -1;
        }
    } else {
        ov->status = OV_UNKNOWN;
        return -1;
    }
    r->pump = after;
    r->dirty = 1;
    return 0;
}

int
overlay_pump(const tx_overlay_t *ov, uint32_t pool_id,
             const pump_state_t **out)
{
    uint8_t i;

    if (ov == NULL || out == NULL) {
        return -1;
    }
    for (i = 0; i < ov->n; i++) {
        if (ov->row[i].pool_id == pool_id) {
            *out = &ov->row[i].pump;
            return 0;
        }
    }
    return -1;
}
