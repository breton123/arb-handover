#include "oracle/oracle.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int
oracle_pump_eq(const pump_state_t *ours, const oracle_pump_t *auth)
{
    if (ours == NULL || auth == NULL) {
        return -1;
    }
    if (ours->reserve_base != auth->reserve_base) {
        return 1;
    }
    if (ours->reserve_quote != auth->reserve_quote) {
        return 1;
    }
    if (ours->virtual_quote != auth->virtual_quote) {
        return 1;
    }
    return 0;
}

int
oracle_diff_state(const compact_state_t *ours, const compact_state_t *auth,
                  oracle_diff_t *out)
{
    uint32_t i;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (ours == NULL || auth == NULL) {
        return -1;
    }
    out->class_ours = ours->cert.exactness_class;
    if (ours->n_pool != auth->n_pool) {
        out->n_pool_mismatch++;
    }
    for (i = 0; i < ours->n_pool && i < auth->n_pool; i++) {
        const pump_state_t *a = &ours->pool[i].pump;
        const pump_state_t *b = &auth->pool[i].pump;

        if (a->reserve_base != b->reserve_base
            || a->reserve_quote != b->reserve_quote
            || a->virtual_quote != b->virtual_quote
            || a->lp_fee_bps != b->lp_fee_bps
            || a->protocol_fee_bps != b->protocol_fee_bps
            || a->creator_fee_bps != b->creator_fee_bps) {
            out->n_pool_mismatch++;
        }
        if (ours->pool[i].incarnation != auth->pool[i].incarnation
            || ours->pool[i].live != auth->pool[i].live) {
            out->n_incarnation_mismatch++;
        }
    }
    if (ours->n_token != auth->n_token) {
        out->n_token_mismatch++;
    }
    for (i = 0; i < ours->n_token && i < auth->n_token; i++) {
        const state_token_t *a = &ours->token[i];
        const state_token_t *b = &auth->token[i];

        if (a->amount != b->amount || a->native_reserve != b->native_reserve
            || a->live != b->live || a->flags != b->flags
            || a->token_program != b->token_program) {
            out->n_token_mismatch++;
        }
        if (a->incarnation != b->incarnation) {
            out->n_incarnation_mismatch++;
        }
    }
    if (ours->n_sys != auth->n_sys) {
        out->n_sys_mismatch++;
    }
    for (i = 0; i < ours->n_sys && i < auth->n_sys; i++) {
        if (ours->sys[i].lamports != auth->sys[i].lamports
            || ours->sys[i].live != auth->sys[i].live) {
            out->n_sys_mismatch++;
        }
        if (ours->sys[i].incarnation != auth->sys[i].incarnation) {
            out->n_incarnation_mismatch++;
        }
    }
    if (out->n_pool_mismatch != 0 || out->n_token_mismatch != 0
        || out->n_sys_mismatch != 0 || out->n_incarnation_mismatch != 0) {
        return ORACLE_MISMATCH;
    }
    return ORACLE_EXACT;
}

int
oracle_diff_overlay(const tx_overlay_t *ov, const compact_state_t *auth,
                    oracle_diff_t *out)
{
    uint8_t i;

    if (ov == NULL || auth == NULL || out == NULL) {
        return -1;
    }
    for (i = 0; i < ov->n; i++) {
        const overlay_row_t *r = &ov->row[i];
        const state_pool_t *p;

        if (!r->dirty) {
            continue;
        }
        if (compact_pool_get(auth, r->pool_id, &p) != 0) {
            out->n_overlay_mismatch++;
            continue;
        }
        if (r->pump.reserve_base != p->pump.reserve_base
            || r->pump.reserve_quote != p->pump.reserve_quote
            || r->pump.virtual_quote != p->pump.virtual_quote) {
            out->n_overlay_mismatch++;
        }
    }
    for (i = 0; i < ov->n_token; i++) {
        const overlay_token_row_t *r = &ov->token[i];
        const state_token_t *t;

        if (!r->dirty) {
            continue;
        }
        if (compact_token_get(auth, r->id, &t) != 0
            || r->tok.amount != t->amount
            || r->tok.native_reserve != t->native_reserve
            || r->tok.live != t->live) {
            out->n_overlay_mismatch++;
        }
    }
    for (i = 0; i < ov->n_sys; i++) {
        const overlay_sys_row_t *r = &ov->sys[i];
        const state_sys_t *s;

        if (!r->dirty) {
            continue;
        }
        if (compact_sys_get(auth, r->id, &s) != 0
            || r->sys.lamports != s->lamports) {
            out->n_overlay_mismatch++;
        }
    }
    return (out->n_overlay_mismatch == 0) ? ORACLE_EXACT : ORACLE_MISMATCH;
}

int
oracle_diff_cert(const state_cert_t *ours, const ordered_tx_t *tx,
                 uint8_t expect_exactness, oracle_diff_t *out)
{
    if (ours == NULL || tx == NULL || out == NULL) {
        return -1;
    }
    if (ours->slot != tx->slot || ours->tx_index != tx->tx_index) {
        out->n_cert_mismatch++;
    }
    if (ours->model_version != STATE_MODEL_VERSION
        || ours->program_hash != STATE_PROGRAM_HASH) {
        out->n_cert_mismatch++;
    }
    if (ours->exactness_class != expect_exactness) {
        out->n_cert_mismatch++;
    }
    if (memcmp(ours->sig, tx->sig, STATE_SIG_LEN) != 0) {
        out->n_cert_mismatch++;
    }
    return (out->n_cert_mismatch == 0) ? ORACLE_EXACT : ORACLE_MISMATCH;
}

uint8_t
oracle_judge(int apply_rc, uint8_t dep_class, uint8_t pre_complete,
             int fields_exact)
{
    if (!pre_complete) {
        if (apply_rc == APPLY_OK && fields_exact) {
            return ORACLE_FAST_CUSTOM_EXACT;
        }
        return ORACLE_FALLBACK_REQUIRED;
    }
    if (apply_rc == APPLY_REJECT && (dep_class == DEP_UNKNOWN
                                    || deps_blocks_fast(dep_class))) {
        return ORACLE_UNKNOWN;
    }
    if (apply_rc == APPLY_REJECT && dep_class == DEP_FALLBACK) {
        return ORACLE_FALLBACK_REQUIRED;
    }
    if (apply_rc == APPLY_IRRELEVANT && fields_exact) {
        return ORACLE_FAST_CUSTOM_EXACT;
    }
    if (apply_rc == APPLY_OK && fields_exact) {
        return ORACLE_FAST_CUSTOM_EXACT;
    }
    return ORACLE_UNEXPLAINED;
}

int
oracle_replay(const compact_state_t *pre, const ordered_tx_t *tx,
              const compact_state_t *certified_post, uint8_t expect_class,
              oracle_diff_t *out)
{
    compact_state_t *work;
    apply_result_t ar;
    int rc, fields;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->class_expect = expect_class;
    if (pre == NULL || tx == NULL || certified_post == NULL) {
        return -1;
    }
    work = malloc(sizeof(*work));
    if (work == NULL) {
        return -1;
    }
    *work = *pre;
    rc = apply_tx(work, tx, &ar);
    out->class_ours = ar.dep_class;
    fields = ORACLE_MISMATCH;
    if (rc == APPLY_OK || rc == APPLY_IRRELEVANT) {
        fields = oracle_diff_state(work, certified_post, out);
        (void)oracle_diff_overlay(&ar.overlay, certified_post, out);
        if (rc == APPLY_OK) {
            (void)oracle_diff_cert(&ar.cert, tx, DEP_FAST_CUSTOM, out);
        }
        if (out->n_overlay_mismatch != 0 || out->n_cert_mismatch != 0) {
            fields = ORACLE_MISMATCH;
        }
    }
    out->class_expect = expect_class;
    out->class_ours = ar.dep_class;
    out->judged = oracle_judge(rc, ar.dep_class, 1,
                               fields == ORACLE_EXACT);
    free(work);
    if (expect_class == DEP_FAST_CUSTOM) {
        return (out->judged == ORACLE_FAST_CUSTOM_EXACT) ? ORACLE_EXACT
                                                         : ORACLE_MISMATCH;
    }
    if (expect_class == DEP_IRRELEVANT) {
        return (rc == APPLY_IRRELEVANT && fields == ORACLE_EXACT)
            ? ORACLE_EXACT
            : ORACLE_REJECTED;
    }
    if (rc == APPLY_REJECT && ar.dep_class == expect_class) {
        return ORACLE_EXACT;
    }
    return ORACLE_REJECTED;
}
