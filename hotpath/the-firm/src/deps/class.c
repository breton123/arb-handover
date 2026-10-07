#include "deps/class.h"

#include <string.h>

/* ComputeBudget111111111111111111111111111111 */
static const uint8_t COMPUTE_BUDGET[32] = {
    0x03, 0x06, 0x46, 0x6f, 0xe5, 0x21, 0x17, 0x32, 0xff, 0xec, 0xad, 0xba,
    0x72, 0xc3, 0x9b, 0xe7, 0xbc, 0x8c, 0xe5, 0xbb, 0xc5, 0xf7, 0x12, 0x6b,
    0x2c, 0x43, 0x9b, 0x3a, 0x40, 0x00, 0x00, 0x00
};

/* Pump AMM global_config PDA. Seed "global_config". */
static const uint8_t PUMP_GLOBAL_CONFIG[32] = {
    0x89, 0x0b, 0xa6, 0x44, 0xfe, 0x1f, 0x55, 0xaa, 0x19, 0xf1, 0x1c, 0xd2,
    0xd2, 0xec, 0x14, 0xd3, 0x23, 0x3b, 0x6e, 0x0a, 0x4b, 0xea, 0xee, 0xf7,
    0x2b, 0x69, 0x85, 0x8e, 0x21, 0xe1, 0x70, 0xd6
};

static int
is_pump_fast(uint8_t kind)
{
    return kind == IX_KIND_PUMP_SELL || kind == IX_KIND_PUMP_BUY_EQ
        || kind == IX_KIND_PUMP_BUY;
}

static int
is_token_fast(uint8_t kind)
{
    return kind == IX_KIND_TOKEN_XFER || kind == IX_KIND_TOKEN_SYNC
        || kind == IX_KIND_TOKEN_CLOSE || kind == IX_KIND_ATA_CREATE
        || kind == IX_KIND_ATA_INIT || kind == IX_KIND_SYS_TRANSFER;
}

static int
pk_zero(const uint8_t *pk)
{
    static const uint8_t z[32];

    return pk == NULL || memcmp(pk, z, 32) == 0;
}

static int
pump_quote_deps_complete(const compact_state_t *st, const ordered_tx_t *tx)
{
    uint16_t i;
    int saw = 0;

    for (i = 0; i < tx->n_ix; i++) {
        const ordered_ix_t *ix = &tx->ix[i];
        const state_pool_t *p;

        if (!is_pump_fast(ix->kind) || !ix->relevant) {
            continue;
        }
        saw = 1;
        if (ix->pool_id == STATE_ACCT_NONE
            || compact_pool_get(st, ix->pool_id, &p) != 0 || !p->live) {
            return 0;
        }
        if (pk_zero(p->vault_base) || pk_zero(p->vault_quote)) {
            return 0;
        }
        if (p->pump.protocol_fee_bps != 0 && ix->fee_proto == STATE_ACCT_NONE
            && tx->acc_n[i] < 11u) {
            return 0;
        }
        if (p->pump.creator_fee_bps != 0 && ix->fee_creator == STATE_ACCT_NONE
            && tx->acc_n[i] < 16u) {
            return 0;
        }
    }
    return saw;
}

static int
eq32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32) == 0;
}

static int
is_compute_budget(const ordered_tx_t *tx, const ordered_ix_t *ix)
{
    if (tx == NULL || ix == NULL || ix->prog >= tx->n_keys) {
        return 0;
    }
    return eq32(tx->key[ix->prog], COMPUTE_BUDGET);
}

static int
is_skip_class(uint8_t c)
{
    return c == DEP_IRRELEVANT || c == DEP_NO_QUOTE_EFFECT;
}

int
deps_blocks_fast(uint8_t c)
{
    return c != DEP_IRRELEVANT && c != DEP_FAST_CUSTOM
        && c != DEP_NO_QUOTE_EFFECT && c != DEP_FALLBACK;
}

const char *
deps_class_name(uint8_t c)
{
    switch (c) {
    case DEP_IRRELEVANT:
        return "DEP_IRRELEVANT";
    case DEP_FAST_CUSTOM:
        return "DEP_FAST_CUSTOM";
    case DEP_FALLBACK:
        return "DEP_FALLBACK";
    case DEP_NO_QUOTE_EFFECT:
        return "DEP_NO_QUOTE_EFFECT";
    case DEP_MISSING_POOL_ROW:
        return "DEP_MISSING_POOL_ROW";
    case DEP_MISSING_BASE_VAULT_ROW:
        return "DEP_MISSING_BASE_VAULT_ROW";
    case DEP_MISSING_QUOTE_VAULT_ROW:
        return "DEP_MISSING_QUOTE_VAULT_ROW";
    case DEP_MISSING_CONFIG:
        return "DEP_MISSING_CONFIG";
    case DEP_MISSING_OTHER_ROW:
        return "DEP_MISSING_OTHER_ROW";
    case DEP_EXTRA_IX_TOUCHES_STATE:
        return "DEP_EXTRA_IX_TOUCHES_STATE";
    case DEP_EXTRA_IX_NO_STATE_OVERLAP:
        return "DEP_EXTRA_IX_NO_STATE_OVERLAP";
    case DEP_UNKNOWN_CPI_EFFECT:
        return "DEP_UNKNOWN_CPI_EFFECT";
    case DEP_UNKNOWN_PUMP_KIND:
        return "DEP_UNKNOWN_PUMP_KIND";
    default:
        return "DEP_UNKNOWN";
    }
}

const char *
deps_need_name(uint8_t need)
{
    switch (need) {
    case DEP_NEED_POOL:
        return "pool";
    case DEP_NEED_VAULT_BASE:
        return "vault_base";
    case DEP_NEED_VAULT_QUOTE:
        return "vault_quote";
    case DEP_NEED_SRC_TOKEN:
        return "src_token";
    case DEP_NEED_DST_TOKEN:
        return "dst_token";
    case DEP_NEED_FEE_PROTO:
        return "fee_proto";
    case DEP_NEED_FEE_CREATOR:
        return "fee_creator";
    case DEP_NEED_SRC_SYS:
        return "src_sys";
    case DEP_NEED_DST_SYS:
        return "dst_sys";
    default:
        return "none";
    }
}

static void
deps_recompute(dep_report_t *r, uint16_t n_ix)
{
    uint16_t i;
    uint8_t first_fail = 0;

    r->n_relevant = 0;
    r->n_fast = 0;
    r->n_fallback = 0;
    r->n_unknown = 0;
    r->tx_class = DEP_IRRELEVANT;
    for (i = 0; i < n_ix && i < STATE_IX_MAX; i++) {
        uint8_t c = r->ix_class[i];

        if (is_skip_class(c)) {
            continue;
        }
        r->n_relevant++;
        if (c == DEP_FAST_CUSTOM) {
            r->n_fast++;
        } else if (c == DEP_FALLBACK) {
            r->n_fallback++;
        } else {
            r->n_unknown++;
            if (first_fail == 0) {
                first_fail = c;
            }
        }
    }
    if (first_fail != 0) {
        r->tx_class = first_fail;
    } else if (r->n_fallback != 0) {
        r->tx_class = DEP_FALLBACK;
    } else if (r->n_fast != 0) {
        r->tx_class = DEP_FAST_CUSTOM;
    }
}

static uint8_t
class_ix(const ordered_tx_t *tx, uint16_t i)
{
    const ordered_ix_t *ix;

    if (tx == NULL || i >= tx->n_ix) {
        return DEP_IRRELEVANT;
    }
    ix = &tx->ix[i];
    if (!ix->relevant) {
        return DEP_IRRELEVANT;
    }
    if (ix->proto == PROTO_PUMP && is_pump_fast(ix->kind)) {
        return DEP_FAST_CUSTOM;
    }
    if (ix->proto == PROTO_PUMP) {
        return DEP_UNKNOWN_PUMP_KIND;
    }
    if ((ix->proto == PROTO_TOKEN || ix->proto == PROTO_ATA
         || ix->proto == PROTO_SYSTEM)
        && is_token_fast(ix->kind)) {
        return DEP_FAST_CUSTOM;
    }
    if (ix->proto == PROTO_TOKEN) {
        return DEP_UNKNOWN_CPI_EFFECT;
    }
    if (ix->proto == PROTO_DLMM) {
        return DEP_FALLBACK;
    }
    if (tx->acc_n[i] == 0 || is_compute_budget(tx, ix)) {
        return DEP_NO_QUOTE_EFFECT;
    }
    return DEP_UNKNOWN;
}

static void
copy32(uint8_t *dst, const uint8_t *src)
{
    if (src != NULL) {
        memcpy(dst, src, 32);
    }
}

static void
set_offend(dep_report_t *r, uint8_t reason, uint16_t ix_i, uint8_t acc_i,
           uint8_t writable, uint8_t present, uint8_t exact,
           const uint8_t *account, const uint8_t *program, uint8_t need)
{
    if (r == NULL || r->have_off) {
        return;
    }
    r->have_off = 1;
    memset(&r->off, 0, sizeof(r->off));
    r->off.reason = reason;
    r->off.ix_index = ix_i;
    r->off.account_index = acc_i;
    r->off.writable = writable;
    r->off.compact_row_present = present;
    r->off.compact_row_exact = exact;
    r->off.required_by = need;
    copy32(r->off.account, account);
    copy32(r->off.program, program);
}

static const uint8_t *
ix_program(const ordered_tx_t *tx, uint16_t i)
{
    const ordered_ix_t *ix = &tx->ix[i];

    if (ix->prog >= tx->n_keys) {
        return NULL;
    }
    return tx->key[ix->prog];
}

void
deps_classify(const ordered_tx_t *tx, dep_report_t *out)
{
    uint16_t i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->tx_class = DEP_IRRELEVANT;
    if (tx == NULL) {
        out->tx_class = DEP_UNKNOWN_CPI_EFFECT;
        return;
    }
    for (i = 0; i < tx->n_ix && i < STATE_IX_MAX; i++) {
        out->ix_class[i] = class_ix(tx, i);
    }
    deps_recompute(out, tx->n_ix);
}

static int
sys_ok(const compact_state_t *st, uint32_t id)
{
    const state_sys_t *s;

    if (id == STATE_ACCT_NONE) {
        return 0;
    }
    if (compact_sys_get(st, id, &s) != 0 || !s->live) {
        return -1;
    }
    return 0;
}

static void
token_meta(const compact_state_t *st, uint32_t id, uint8_t *present,
           uint8_t *exact, const uint8_t **pk)
{
    const state_token_t *t;

    *present = 0;
    *exact = 0;
    *pk = NULL;
    if (id == STATE_ACCT_NONE || compact_token_get(st, id, &t) != 0) {
        return;
    }
    *present = 1;
    *pk = t->pubkey;
    *exact = (uint8_t)(t->live && t->extensions_mask == 0);
}

static uint8_t
fail_token(const compact_state_t *st, uint32_t id, uint8_t allow_dead,
           uint8_t missing_code)
{
    const state_token_t *t;

    if (id == STATE_ACCT_NONE) {
        return 0;
    }
    if (compact_token_get(st, id, &t) != 0) {
        return missing_code;
    }
    if (t->extensions_mask != 0 || (!t->live && !allow_dead)) {
        return DEP_MISSING_OTHER_ROW;
    }
    return 0;
}

static int
ordered_key_writable(const ordered_tx_t *tx, uint8_t idx)
{
    uint16_t nws;
    uint16_t first_ro_u;

    if (tx == NULL || idx >= tx->n_keys) {
        return 0;
    }
    if (!tx->hdr_ok) {
        return -1;
    }
    if (idx < tx->n_static) {
        if (idx < tx->nsig) {
            if (tx->nro_signed > tx->nsig) {
                return -1;
            }
            nws = (uint16_t)(tx->nsig - tx->nro_signed);
            return idx < nws;
        }
        if (tx->nro_unsigned > tx->n_static) {
            return -1;
        }
        first_ro_u = (uint16_t)(tx->n_static - tx->nro_unsigned);
        return idx < first_ro_u;
    }
    return (uint16_t)(idx - tx->n_static) < tx->n_lut_w;
}

static int
pk_in_pump_deps(const compact_state_t *st, const ordered_tx_t *tx,
                const uint8_t *pk)
{
    uint16_t i;

    if (pk == NULL) {
        return 0;
    }
    for (i = 0; i < tx->n_ix; i++) {
        const ordered_ix_t *ix = &tx->ix[i];
        const state_pool_t *p;
        const state_token_t *t;

        if (!is_pump_fast(ix->kind) || ix->pool_id == STATE_ACCT_NONE) {
            continue;
        }
        if (compact_pool_get(st, ix->pool_id, &p) != 0) {
            continue;
        }
        if (eq32(pk, p->pubkey) || eq32(pk, p->vault_base)
            || eq32(pk, p->vault_quote) || eq32(pk, PUMP_GLOBAL_CONFIG)) {
            return 1;
        }
        if (tx->acc_n[i] > 9u) {
            uint8_t ai = tx->acc_ix[i][9];

            if (ai < tx->n_keys && eq32(pk, tx->key[ai])) {
                return 1;
            }
        }
        if (tx->acc_n[i] > 10u) {
            uint8_t ai = tx->acc_ix[i][10];

            if (ai < tx->n_keys && eq32(pk, tx->key[ai])) {
                return 1;
            }
        }
        if (ix->fee_proto != STATE_ACCT_NONE
            && compact_token_get(st, ix->fee_proto, &t) == 0
            && eq32(pk, t->pubkey)) {
            return 1;
        }
        if (ix->fee_creator != STATE_ACCT_NONE
            && compact_token_get(st, ix->fee_creator, &t) == 0
            && eq32(pk, t->pubkey)) {
            return 1;
        }
    }
    return 0;
}

static int
extra_writable_overlap(const compact_state_t *st, const ordered_tx_t *tx,
                       uint16_t i, dep_report_t *r)
{
    uint8_t a;
    int saw_unknown = 0;

    if (tx->acc_n[i] == 0) {
        return 0;
    }
    if (!tx->hdr_ok) {
        return -1;
    }
    for (a = 0; a < tx->acc_n[i]; a++) {
        uint8_t idx = tx->acc_ix[i][a];
        int w;
        uint8_t present = 0, exact = 0;
        uint32_t tid;

        if (idx >= tx->n_keys) {
            continue;
        }
        w = ordered_key_writable(tx, idx);
        if (w < 0) {
            saw_unknown = 1;
            continue;
        }
        if (w == 0) {
            continue;
        }
        if (!pk_in_pump_deps(st, tx, tx->key[idx])) {
            continue;
        }
        if (compact_token_find(st, tx->key[idx], &tid) == 0) {
            const state_token_t *t;

            if (compact_token_get(st, tid, &t) == 0) {
                present = 1;
                exact = (uint8_t)(t->live && t->extensions_mask == 0);
            }
        } else if (compact_pool_find(st, tx->key[idx], &tid) == 0) {
            present = 1;
            exact = 1;
        }
        set_offend(r, DEP_EXTRA_IX_TOUCHES_STATE, i, a, 1, present, exact,
                   tx->key[idx], ix_program(tx, i), DEP_NEED_NONE);
        return 1;
    }
    if (saw_unknown) {
        return -1;
    }
    return 0;
}

static void
fail_ix(dep_report_t *r, uint16_t i, uint8_t reason)
{
    r->ix_class[i] = reason;
}

int
deps_lookup(const compact_state_t *st, const ordered_tx_t *tx,
            dep_report_t *inout)
{
    uint16_t i;

    if (st == NULL || tx == NULL || inout == NULL) {
        return -1;
    }
    for (i = 0; i < tx->n_ix && i < STATE_IX_MAX; i++) {
        const ordered_ix_t *ix = &tx->ix[i];
        uint8_t allow_dead, reason, present, exact;
        const uint8_t *pk;
        const uint8_t *prog;
        int ov;

        if (!ix->relevant || is_skip_class(inout->ix_class[i])) {
            continue;
        }
        prog = ix_program(tx, i);
        if (inout->ix_class[i] == DEP_UNKNOWN_PUMP_KIND) {
            set_offend(inout, DEP_UNKNOWN_PUMP_KIND, i, 0, 0, 0, 0, NULL,
                       prog, DEP_NEED_NONE);
            continue;
        }
        if (inout->ix_class[i] == DEP_UNKNOWN) {
            ov = extra_writable_overlap(st, tx, i, inout);
            if (ov == 1) {
                fail_ix(inout, i, DEP_EXTRA_IX_TOUCHES_STATE);
                continue;
            }
            if (ov < 0) {
                set_offend(inout, DEP_UNKNOWN_CPI_EFFECT, i, 0, 0, 0, 0,
                           NULL, prog, DEP_NEED_NONE);
                fail_ix(inout, i, DEP_UNKNOWN_CPI_EFFECT);
                continue;
            }
            if (!pump_quote_deps_complete(st, tx)) {
                set_offend(inout, DEP_MISSING_CONFIG, i, 0, 0, 0, 0, NULL,
                           prog, DEP_NEED_NONE);
                fail_ix(inout, i, DEP_MISSING_CONFIG);
                continue;
            }
            /* writable ∩ complete Pump dep set == ∅ */
            fail_ix(inout, i, DEP_NO_QUOTE_EFFECT);
            continue;
        }
        if (inout->ix_class[i] != DEP_FAST_CUSTOM) {
            continue;
        }
        allow_dead = (uint8_t)(ix->kind == IX_KIND_ATA_CREATE
                               || ix->kind == IX_KIND_ATA_INIT);
        if (is_pump_fast(ix->kind)) {
            const state_pool_t *p;

            if (ix->pool_id == STATE_ACCT_NONE
                || compact_pool_get(st, ix->pool_id, &p) != 0 || !p->live) {
                set_offend(inout, DEP_MISSING_POOL_ROW, i, 0, 1, 0, 0,
                           NULL, prog, DEP_NEED_POOL);
                fail_ix(inout, i, DEP_MISSING_POOL_ROW);
                continue;
            }
            reason = fail_token(st, ix->vault_base, 0,
                                DEP_MISSING_BASE_VAULT_ROW);
            if (reason != 0) {
                token_meta(st, ix->vault_base, &present, &exact, &pk);
                if (pk == NULL) {
                    pk = p->vault_base;
                }
                set_offend(inout, reason, i, 7, 1, present, exact, pk, prog,
                           DEP_NEED_VAULT_BASE);
                fail_ix(inout, i, reason);
                continue;
            }
            reason = fail_token(st, ix->vault_quote, 0,
                                DEP_MISSING_QUOTE_VAULT_ROW);
            if (reason != 0) {
                token_meta(st, ix->vault_quote, &present, &exact, &pk);
                if (pk == NULL) {
                    pk = p->vault_quote;
                }
                set_offend(inout, reason, i, 8, 1, present, exact, pk, prog,
                           DEP_NEED_VAULT_QUOTE);
                fail_ix(inout, i, reason);
                continue;
            }
            /*
             * User ATAs, fee destinations, and sys accounts are not
             * inputs to the Pump quote kernel. overlay_apply_pump_tokens
             * no-ops missing token ids. Requiring those CompactState
             * rows dirties EXACT pools on ATA/user-balance misses.
             */
            continue;
        } else if (ix->proto == PROTO_PUMP && ix->pool_id != STATE_ACCT_NONE) {
            const state_pool_t *p;

            if (compact_pool_get(st, ix->pool_id, &p) != 0 || !p->live) {
                set_offend(inout, DEP_MISSING_POOL_ROW, i, 0, 1, 0, 0,
                           NULL, prog, DEP_NEED_POOL);
                fail_ix(inout, i, DEP_MISSING_POOL_ROW);
                continue;
            }
        }
        reason = 0;
        if (ix->kind != IX_KIND_ATA_CREATE && ix->kind != IX_KIND_ATA_INIT) {
            reason = fail_token(st, ix->src_token, 0, DEP_MISSING_OTHER_ROW);
            if (reason != 0) {
                token_meta(st, ix->src_token, &present, &exact, &pk);
                set_offend(inout, reason, i, 0, 1, present, exact, pk, prog,
                           DEP_NEED_SRC_TOKEN);
                fail_ix(inout, i, reason);
                continue;
            }
        }
        reason = fail_token(st, ix->dst_token, allow_dead,
                            DEP_MISSING_OTHER_ROW);
        if (reason == 0) {
            reason = fail_token(st, ix->fee_proto, 0, DEP_MISSING_CONFIG);
        }
        if (reason == 0) {
            reason = fail_token(st, ix->fee_creator, 0, DEP_MISSING_OTHER_ROW);
        }
        if (reason == 0 && ix->proto == PROTO_SYSTEM
            && sys_ok(st, ix->src_sys) != 0) {
            reason = DEP_MISSING_OTHER_ROW;
        }
        if (reason == 0 && ix->proto == PROTO_SYSTEM
            && sys_ok(st, ix->dst_sys) != 0) {
            reason = DEP_MISSING_OTHER_ROW;
        }
        if (reason != 0) {
            uint32_t id = ix->dst_token;
            uint8_t need = DEP_NEED_DST_TOKEN;

            if (fail_token(st, ix->dst_token, allow_dead,
                           DEP_MISSING_OTHER_ROW) == 0) {
                if (fail_token(st, ix->fee_proto, 0, DEP_MISSING_CONFIG)
                    != 0) {
                    id = ix->fee_proto;
                    need = DEP_NEED_FEE_PROTO;
                } else if (fail_token(st, ix->fee_creator, 0,
                                      DEP_MISSING_OTHER_ROW) != 0) {
                    id = ix->fee_creator;
                    need = DEP_NEED_FEE_CREATOR;
                } else if (sys_ok(st, ix->src_sys) != 0) {
                    need = DEP_NEED_SRC_SYS;
                    id = ix->src_sys;
                } else {
                    need = DEP_NEED_DST_SYS;
                    id = ix->dst_sys;
                }
            }
            token_meta(st, id, &present, &exact, &pk);
            set_offend(inout, reason, i, 0, 1, present, exact, pk, prog,
                       need);
            fail_ix(inout, i, reason);
        }
    }
    for (i = 0; i < tx->n_ix && i < STATE_IX_MAX; i++) {
        if (inout->ix_class[i] == DEP_UNKNOWN) {
            set_offend(inout, DEP_UNKNOWN_CPI_EFFECT, i, 0, 0, 0, 0, NULL,
                       ix_program(tx, i), DEP_NEED_NONE);
            fail_ix(inout, i, DEP_UNKNOWN_CPI_EFFECT);
        }
    }
    deps_recompute(inout, tx->n_ix);
    return 0;
}
