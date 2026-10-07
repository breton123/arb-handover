#include "wire/pipeline.h"

#include "deps/class.h"
#include "ingress/decode.h"
#include "state/cert.h"
#include "ingress/resolve.h"
#include "net/time.h"
#include "publish/publish.h"
#include "shred/complete.h"
#include "transitions/token.h"

#include <string.h>

static uint64_t
ns_of(double hz, uint64_t t0, uint64_t t1)
{
    if (hz <= 0.0 || t1 < t0) {
        return 0;
    }
    return (uint64_t)((double)(t1 - t0) * 1e9 / hz);
}

static uint8_t
pre_fail(const compact_state_t *st, const ordered_tx_t *tx)
{
    uint16_t i;

    for (i = 0; i < tx->n_ix; i++) {
        const ordered_ix_t *ix = &tx->ix[i];
        const state_pool_t *p;

        if (ix->proto != PROTO_PUMP
            || (ix->kind != IX_KIND_PUMP_SELL && ix->kind != IX_KIND_PUMP_BUY_EQ
                && ix->kind != IX_KIND_PUMP_BUY)) {
            continue;
        }
        if (ix->pool_id == STATE_ACCT_NONE
            || compact_pool_get(st, ix->pool_id, &p) != 0) {
            return WIRE_FAIL_RESOLVE;
        }
        if ((p->auth_bits & POOL_AUTH_VQ) == 0) {
            return WIRE_FAIL_PRE_VQ;
        }
        if ((p->auth_bits & POOL_AUTH_FEE) == 0) {
            return WIRE_FAIL_PRE_FEE;
        }
        if ((p->auth_bits & POOL_AUTH_VAULT) == 0) {
            return WIRE_FAIL_PRE_VAULT;
        }
        if ((p->auth_bits & POOL_AUTH_MINT) == 0) {
            return WIRE_FAIL_PRE_MINT;
        }
    }
    return WIRE_FAIL_NONE;
}

const char *
wire_fail_name(uint8_t fail)
{
    switch (fail) {
    case WIRE_FAIL_NONE:
        return "none";
    case WIRE_FAIL_SHRED:
        return "shred";
    case WIRE_FAIL_TX_WAIT:
        return "tx_incomplete";
    case WIRE_FAIL_DECODE:
        return "decode";
    case WIRE_FAIL_ALT:
        return "alt";
    case WIRE_FAIL_RESOLVE:
        return "resolve";
    case WIRE_FAIL_PRE_VQ:
        return "pre_virtual";
    case WIRE_FAIL_PRE_FEE:
        return "pre_fee";
    case WIRE_FAIL_PRE_VAULT:
        return "pre_vault";
    case WIRE_FAIL_PRE_MINT:
        return "pre_mint";
    case WIRE_FAIL_FALLBACK:
        return "class_fallback";
    case WIRE_FAIL_UNKNOWN:
        return "class_unknown";
    case WIRE_FAIL_APPLY:
        return "apply";
    case WIRE_FAIL_DLMM:
        return "dlmm";
    default:
        return "other";
    }
}

int
wire_to_state(const wire_in_t *in, wire_out_t *out, double tsc_hz)
{
    uint64_t t_rx, t_tx, t_dec, t_res, t_cls, t_pre, t_app, t_cert, t_pub;
    uint64_t t_pre_ready, slot = 0;
    const uint8_t *txb = NULL;
    uint32_t txl = 0;
    int src, drc;
    dep_report_t dep;
    uint16_t i;

    if (in == NULL || out == NULL || in->st == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    ordered_tx_clear(&out->tx);
    t_rx = in->tsc_rx != 0 ? in->tsc_rx : net_rdtscp();
    t_pre_ready = in->tsc_pre_ready != 0 ? in->tsc_pre_ready : t_rx;
    out->ns[WIRE_ST_PACKET] = 0;
    out->tsc_pre_ready = t_pre_ready;

    if (in->framed != NULL && in->framed_len != 0) {
        txb = in->framed;
        txl = in->framed_len;
        t_tx = in->tsc_tx_ready != 0 ? in->tsc_tx_ready : net_rdtscp();
        src = SHRED_TX_OK;
    } else {
        src = shred_first_tx(in->pkt, in->pkt_len, &slot, &txb, &txl);
        t_tx = in->tsc_tx_ready != 0 ? in->tsc_tx_ready : net_rdtscp();
    }
    out->tsc_tx_ready = t_tx;
    out->ns[WIRE_ST_TX_COMPLETE] = ns_of(tsc_hz, t_rx, t_tx);
    if (src == SHRED_TX_WAIT) {
        out->fail = WIRE_FAIL_TX_WAIT;
        out->admit = DEP_FALLBACK;
        out->ns[WIRE_ST_E2E] = out->ns[WIRE_ST_TX_COMPLETE];
        return 0;
    }
    if (src != SHRED_TX_OK) {
        out->fail = WIRE_FAIL_SHRED;
        out->admit = DEP_UNKNOWN;
        out->ns[WIRE_ST_E2E] = out->ns[WIRE_ST_TX_COMPLETE];
        return 0;
    }

    drc = ingress_decode_tx(txb, txl, slot, &out->tx);
    t_dec = net_rdtscp();
    out->ns[WIRE_ST_ORDERED] = ns_of(tsc_hz, t_tx, t_dec);
    if (drc == DEC_ALT) {
        out->fail = WIRE_FAIL_ALT;
        out->admit = DEP_FALLBACK;
        out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_dec);
        return 0;
    }
    if (drc != DEC_OK) {
        out->fail = WIRE_FAIL_DECODE;
        out->admit = DEP_UNKNOWN;
        out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_dec);
        return 0;
    }

    (void)ingress_resolve(in->st, &out->tx);
    t_res = net_rdtscp();
    out->ns[WIRE_ST_RESOLVE] = ns_of(tsc_hz, t_dec, t_res);

    deps_classify(&out->tx, &dep);
    t_cls = net_rdtscp();
    out->ns[WIRE_ST_CLASSIFY] = ns_of(tsc_hz, t_res, t_cls);
    if (in->st != NULL) {
        (void)deps_lookup(in->st, &out->tx, &dep);
    }
    out->admit = dep.tx_class;
    for (i = 0; i < out->tx.n_ix; i++) {
        if (out->tx.ix[i].proto == PROTO_DLMM) {
            out->fail = WIRE_FAIL_DLMM;
            out->admit = DEP_FALLBACK;
            out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_cls);
            return 0;
        }
    }
    if (deps_blocks_fast(dep.tx_class)) {
        out->fail = WIRE_FAIL_UNKNOWN;
        out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_cls);
        return 0;
    }
    if (dep.tx_class == DEP_FALLBACK) {
        out->fail = WIRE_FAIL_FALLBACK;
        out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_cls);
        return 0;
    }

    out->fail = pre_fail(in->st, &out->tx);
    t_pre = net_rdtscp();
    out->ns[WIRE_ST_PRESTATE] = ns_of(tsc_hz, t_cls, t_pre);
    out->pre_complete = (uint8_t)(out->fail == WIRE_FAIL_NONE);
    out->tx_before_pre = (uint8_t)(t_tx < t_pre_ready);
    if (!out->pre_complete) {
        out->admit = DEP_FALLBACK;
        out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_pre);
        return 0;
    }

    {
        tx_overlay_t *ov = &out->ar.overlay;
        uint16_t k;

        overlay_clear(ov);
        if (deps_lookup(in->st, &out->tx, &dep) != 0
            || dep.tx_class != DEP_FAST_CUSTOM
            || overlay_begin(ov, in->st, &out->tx) != 0) {
            t_app = net_rdtscp();
            out->ns[WIRE_ST_APPLY] = ns_of(tsc_hz, t_pre, t_app);
            out->fail = WIRE_FAIL_APPLY;
            out->admit = dep.tx_class != 0 ? dep.tx_class : DEP_UNKNOWN;
            out->apply_rc = APPLY_REJECT;
            out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_app);
            return 0;
        }
        for (k = 0; k < out->tx.n_ix; k++) {
            pump_swap_result_t res;
            const pump_state_t *after;
            const ordered_ix_t *ix = &out->tx.ix[k];

            if (!ix->relevant || dep.ix_class[k] != DEP_FAST_CUSTOM) {
                continue;
            }
            if (ix->kind == IX_KIND_PUMP_SELL || ix->kind == IX_KIND_PUMP_BUY_EQ
                || ix->kind == IX_KIND_PUMP_BUY) {
                if (overlay_apply_pump_ix(ov, in->st, ix, &res) != 0
                    || overlay_pump(ov, ix->pool_id, &after) != 0
                    || overlay_apply_pump_tokens(ov, in->st, ix, &res, after)
                        != 0) {
                    t_app = net_rdtscp();
                    out->ns[WIRE_ST_APPLY] = ns_of(tsc_hz, t_pre, t_app);
                    out->fail = WIRE_FAIL_APPLY;
                    out->apply_rc = APPLY_REJECT;
                    out->admit = DEP_UNKNOWN;
                    out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_app);
                    return 0;
                }
            } else if (overlay_apply_token_ix(ov, in->st, ix) != 0) {
                t_app = net_rdtscp();
                out->ns[WIRE_ST_APPLY] = ns_of(tsc_hz, t_pre, t_app);
                out->fail = WIRE_FAIL_APPLY;
                out->apply_rc = APPLY_REJECT;
                out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_app);
                return 0;
            }
        }
        t_app = net_rdtscp();
        out->ns[WIRE_ST_APPLY] = ns_of(tsc_hz, t_pre, t_app);
        state_cert_fill(&out->ar.cert, in->st->bank.version, &out->tx,
                        DEP_FAST_CUSTOM,
                        state_dep_hash(&out->tx, dep.ix_class, out->tx.n_ix));
        t_cert = net_rdtscp();
        out->ns[WIRE_ST_CERT] = ns_of(tsc_hz, t_app, t_cert);
        if (state_publish(in->st, ov, &out->tx, &dep) != 0) {
            t_pub = net_rdtscp();
            out->ns[WIRE_ST_PUBLISH] = ns_of(tsc_hz, t_cert, t_pub);
            out->fail = WIRE_FAIL_APPLY;
            out->apply_rc = APPLY_REJECT;
            out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_pub);
            return 0;
        }
        t_pub = net_rdtscp();
        out->ns[WIRE_ST_PUBLISH] = ns_of(tsc_hz, t_cert, t_pub);
        out->tsc_publish = t_pub;
        out->ar.cert = in->st->cert;
        out->ar.code = APPLY_OK;
        out->apply_rc = APPLY_OK;
        out->admit = DEP_FAST_CUSTOM;
        out->fail = WIRE_FAIL_NONE;
        out->ns[WIRE_ST_E2E] = ns_of(tsc_hz, t_rx, t_pub);
    }
    return 0;
}
