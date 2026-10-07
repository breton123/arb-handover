#include "publish/publish.h"

#include <stddef.h>

static int
validate(const compact_state_t *st, const tx_overlay_t *ov)
{
    uint8_t i;

    for (i = 0; i < ov->n; i++) {
        const overlay_row_t *r = &ov->row[i];
        const state_pool_t *p;

        if (!r->dirty) {
            continue;
        }
        if (compact_pool_get(st, r->pool_id, &p) != 0 || !p->live
            || p->incarnation != r->incarnation_base) {
            return -1;
        }
    }
    for (i = 0; i < ov->n_token; i++) {
        const overlay_token_row_t *r = &ov->token[i];
        const state_token_t *t;

        if (!r->dirty) {
            continue;
        }
        if (compact_token_get(st, r->id, &t) != 0
            || t->incarnation != r->incarnation_base) {
            return -1;
        }
    }
    for (i = 0; i < ov->n_sys; i++) {
        const overlay_sys_row_t *r = &ov->sys[i];
        const state_sys_t *s;

        if (!r->dirty) {
            continue;
        }
        if (compact_sys_get(st, r->id, &s) != 0 || !s->live
            || s->incarnation != r->incarnation_base) {
            return -1;
        }
    }
    return 0;
}

static void
commit(compact_state_t *st, const tx_overlay_t *ov)
{
    uint8_t i;

    for (i = 0; i < ov->n; i++) {
        const overlay_row_t *r = &ov->row[i];
        if (!r->dirty) {
            continue;
        }
        st->pool[r->pool_id].pump = r->pump;
        st->pool[r->pool_id].incarnation++;
    }
    for (i = 0; i < ov->n_token; i++) {
        const overlay_token_row_t *r = &ov->token[i];
        if (!r->dirty) {
            continue;
        }
        st->token[r->id] = r->tok;
        st->token[r->id].incarnation = r->incarnation_base + 1;
    }
    for (i = 0; i < ov->n_sys; i++) {
        const overlay_sys_row_t *r = &ov->sys[i];
        if (!r->dirty) {
            continue;
        }
        st->sys[r->id] = r->sys;
        st->sys[r->id].incarnation = r->incarnation_base + 1;
    }
}

static void
advance_bank(compact_state_t *st, const ordered_tx_t *tx,
             const dep_report_t *dep)
{
    uint64_t gen = st->bank.version;
    uint64_t h = 0;

    if (dep != NULL) {
        h = state_dep_hash(tx, dep->ix_class, tx->n_ix);
        state_cert_fill(&st->cert, gen, tx, dep->tx_class, h);
    } else {
        state_cert_fill(&st->cert, gen, tx, DEP_UNKNOWN, 0);
    }
    st->bank.slot = tx->slot;
    st->bank.entry_index = tx->entry_index;
    st->bank.tx_index = tx->tx_index;
    st->bank.version++;
}

int
state_publish(compact_state_t *st, const tx_overlay_t *ov,
              const ordered_tx_t *tx, const dep_report_t *dep)
{
    if (st == NULL || ov == NULL || tx == NULL) {
        return -1;
    }
    if (ov->status != OV_OK) {
        return -1;
    }
    if (validate(st, ov) != 0) {
        return -1;
    }
    commit(st, ov);
    advance_bank(st, tx, dep);
    return 0;
}

int
state_publish_irrelevant(compact_state_t *st, const ordered_tx_t *tx,
                         const dep_report_t *dep)
{
    if (st == NULL || tx == NULL) {
        return -1;
    }
    advance_bank(st, tx, dep);
    return 0;
}
