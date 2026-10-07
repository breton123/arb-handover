#include "transitions/apply.h"

#include "publish/publish.h"
#include "transitions/token.h"

#include <string.h>

static int
is_pump_kind(uint8_t kind)
{
    return kind == IX_KIND_PUMP_SELL || kind == IX_KIND_PUMP_BUY_EQ
        || kind == IX_KIND_PUMP_BUY;
}

const char *
apply_step_name(uint8_t step)
{
    switch (step) {
    case APPLY_STEP_CLASS:
        return "LOOKUP_TX_CLASS";
    case APPLY_STEP_BEGIN:
        return "OVERLAY_BEGIN";
    case APPLY_STEP_IX_CLASS:
        return "OVERLAY_IX_CLASS";
    case APPLY_STEP_PUMP:
        return "PUMP_IX";
    case APPLY_STEP_PUMP_TOK:
        return "PUMP_TOKENS";
    case APPLY_STEP_TOKEN:
        return "TOKEN_IX";
    case APPLY_STEP_PUBLISH:
        return "PUBLISH";
    default:
        return "NONE";
    }
}

int
apply_tx(compact_state_t *st, const ordered_tx_t *tx, apply_result_t *out)
{
    dep_report_t dep;
    uint16_t i;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    overlay_clear(&out->overlay);
    if (st == NULL || tx == NULL) {
        out->code = APPLY_REJECT;
        out->dep_class = DEP_UNKNOWN;
        return APPLY_REJECT;
    }

    deps_classify(tx, &dep);
    out->dep_class = dep.tx_class;
    for (i = 0; i < tx->n_ix && i < STATE_IX_MAX; i++) {
        if (dep.ix_class[i] == DEP_NO_QUOTE_EFFECT) {
            out->n_no_quote_ix++;
        }
    }

    if (tx->exec_failed) {
        out->code = APPLY_ABORTED;
        return APPLY_ABORTED;
    }

    if (dep.tx_class == DEP_IRRELEVANT || dep.tx_class == DEP_NO_QUOTE_EFFECT) {
        if (state_publish_irrelevant(st, tx, &dep) != 0) {
            out->code = APPLY_REJECT;
            return APPLY_REJECT;
        }
        out->cert = st->cert;
        out->published_version = st->bank.version;
        out->code = APPLY_IRRELEVANT;
        return APPLY_IRRELEVANT;
    }

    if (deps_lookup(st, tx, &dep) != 0) {
        out->code = APPLY_REJECT;
        out->dep_class = DEP_UNKNOWN_CPI_EFFECT;
        return APPLY_REJECT;
    }
    out->dep_class = dep.tx_class;
    out->have_off = dep.have_off;
    out->off = dep.off;
    memcpy(out->ix_class, dep.ix_class, sizeof(out->ix_class));
    out->n_no_quote_ix = 0;
    for (i = 0; i < tx->n_ix && i < STATE_IX_MAX; i++) {
        if (dep.ix_class[i] == DEP_NO_QUOTE_EFFECT) {
            out->n_no_quote_ix++;
        }
    }

    if (dep.tx_class != DEP_FAST_CUSTOM) {
        out->fail_step = APPLY_STEP_CLASS;
        out->code = APPLY_REJECT;
        return APPLY_REJECT;
    }

    if (overlay_begin(&out->overlay, st, tx) != 0) {
        out->fail_step = APPLY_STEP_BEGIN;
        out->code = APPLY_REJECT;
        return APPLY_REJECT;
    }
    for (i = 0; i < tx->n_ix; i++) {
        pump_swap_result_t res;
        const ordered_ix_t *ix = &tx->ix[i];
        const pump_state_t *after;

        if (!ix->relevant || dep.ix_class[i] == DEP_IRRELEVANT
            || dep.ix_class[i] == DEP_NO_QUOTE_EFFECT) {
            continue;
        }
        if (dep.ix_class[i] != DEP_FAST_CUSTOM) {
            out->overlay.status = OV_UNKNOWN;
            out->fail_step = APPLY_STEP_IX_CLASS;
            out->fail_ix = i;
            out->code = APPLY_REJECT;
            return APPLY_REJECT;
        }
        if (is_pump_kind(ix->kind)) {
            if (overlay_apply_pump_ix(&out->overlay, st, ix, &res) != 0) {
                out->fail_step = APPLY_STEP_PUMP;
                out->fail_ix = i;
                out->code = APPLY_REJECT;
                return APPLY_REJECT;
            }
            if (overlay_pump(&out->overlay, ix->pool_id, &after) != 0) {
                out->fail_step = APPLY_STEP_PUMP;
                out->fail_ix = i;
                out->code = APPLY_REJECT;
                return APPLY_REJECT;
            }
            if (overlay_apply_pump_tokens(&out->overlay, st, ix, &res,
                                          after) != 0) {
                out->fail_step = APPLY_STEP_PUMP_TOK;
                out->fail_ix = i;
                out->code = APPLY_REJECT;
                return APPLY_REJECT;
            }
        } else if (overlay_apply_token_ix(&out->overlay, st, ix) != 0) {
            out->fail_step = APPLY_STEP_TOKEN;
            out->fail_ix = i;
            out->code = APPLY_REJECT;
            return APPLY_REJECT;
        }
    }
    if (state_publish(st, &out->overlay, tx, &dep) != 0) {
        out->fail_step = APPLY_STEP_PUBLISH;
        out->code = APPLY_REJECT;
        return APPLY_REJECT;
    }
    out->cert = st->cert;
    out->published_version = st->bank.version;
    out->code = APPLY_OK;
    return APPLY_OK;
}
