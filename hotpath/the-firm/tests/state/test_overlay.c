#include "ingress/tx.h"
#include "oracle/oracle.h"
#include "state/compact.h"
#include "transitions/apply.h"
#include "transitions/overlay.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL  %s\n", (msg)); \
            g_fail++;                             \
        } else {                                  \
            printf("ok    %s\n", (msg));          \
        }                                         \
    } while (0)

static pump_state_t
base_pool(void)
{
    pump_state_t s;

    memset(&s, 0, sizeof(s));
    s.reserve_base = 1000000;
    s.reserve_quote = 1000000;
    s.lp_fee_bps = 20;
    s.protocol_fee_bps = 5;
    return s;
}

static void
leg(ordered_ix_t *ix, uint32_t pool, uint8_t kind, uint64_t ain, uint8_t dir)
{
    memset(ix, 0, sizeof(*ix));
    ix->proto = PROTO_PUMP;
    ix->kind = kind;
    ix->relevant = 1;
    ix->direction = dir;
    ix->pool_id = pool;
    ix->src_token = STATE_ACCT_NONE;
    ix->dst_token = STATE_ACCT_NONE;
    ix->vault_base = STATE_ACCT_NONE;
    ix->vault_quote = STATE_ACCT_NONE;
    ix->fee_proto = STATE_ACCT_NONE;
    ix->fee_creator = STATE_ACCT_NONE;
    ix->src_sys = STATE_ACCT_NONE;
    ix->dst_sys = STATE_ACCT_NONE;
    ix->amount_in = ain;
    ix->min_out = 0;
}

int
main(void)
{
    compact_state_t st;
    compact_state_t before;
    ordered_tx_t tx;
    apply_result_t ar;
    pump_state_t seed = base_pool();
    pump_state_t seq, tmp;
    pump_swap_ix_t pix;
    pump_swap_result_t res;
    const pump_state_t *ovp;
    uint32_t pid = 0;
    oracle_pump_t auth;

    compact_state_clear(&st);
    CHECK(compact_pool_add_pump(&st, NULL, &seed, &pid) == 0, "add pool");
    before = st;

    ordered_tx_clear(&tx);
    tx.slot = 10;
    tx.tx_index = 1;
    tx.n_ix = 2;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_SELL, 1000, PUMP_DIR_BASE_TO_QUOTE);
    leg(&tx.ix[1], pid, IX_KIND_PUMP_SELL, 2000, PUMP_DIR_BASE_TO_QUOTE);

    seq = seed;
    pix.amount_in = 1000;
    pix.min_amount_out = 0;
    pix.direction = PUMP_DIR_BASE_TO_QUOTE;
    CHECK(pump_apply_swap(&seq, &pix, &tmp, &res) == 0, "seq 1");
    seq = tmp;
    pix.amount_in = 2000;
    CHECK(pump_apply_swap(&seq, &pix, &tmp, &res) == 0, "seq 2");
    seq = tmp;

    CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK, "multi-cpi apply");
    CHECK(st.pool[pid].pump.reserve_base == seq.reserve_base
              && st.pool[pid].pump.reserve_quote == seq.reserve_quote,
          "repeated visit uses provisional state");
    CHECK(st.pool[pid].incarnation == before.pool[pid].incarnation + 1,
          "incarnation bumped once per tx");
    CHECK(st.bank.version == 1 && st.bank.slot == 10, "bank identity");
    CHECK(overlay_pump(&ar.overlay, pid, &ovp) == 0
              && ovp->reserve_base == seq.reserve_base,
          "overlay matches published");

    auth.reserve_base = seq.reserve_base;
    auth.reserve_quote = seq.reserve_quote;
    auth.virtual_quote = seq.virtual_quote;
    auth.slot = 10;
    CHECK(oracle_pump_eq(&st.pool[pid].pump, &auth) == 0, "oracle match");
    CHECK(ar.cert.exactness_class == DEP_FAST_CUSTOM, "cert FAST_CUSTOM");
    CHECK(ar.cert.model_version == STATE_MODEL_VERSION, "cert model");
    CHECK(ar.cert.program_hash == STATE_PROGRAM_HASH, "cert program");
    CHECK(ar.cert.dependency_hash != 0, "cert dep hash");

    /* rollback: second leg min_out impossible */
    compact_state_clear(&st);
    CHECK(compact_pool_add_pump(&st, NULL, &seed, &pid) == 0, "reseed");
    before = st;
    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_SELL, 1000, PUMP_DIR_BASE_TO_QUOTE);
    leg(&tx.ix[1], pid, IX_KIND_PUMP_SELL, 2000, PUMP_DIR_BASE_TO_QUOTE);
    tx.ix[1].min_out = 1000000000ull;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_REJECT, "failed tx rejected");
    CHECK(memcmp(&st.pool[pid].pump, &before.pool[pid].pump,
                 sizeof(pump_state_t)) == 0,
          "canonical unchanged on rollback");
    CHECK(st.bank.version == 0, "no version on reject");

    /* exact-out buy is FAST_CUSTOM */
    compact_state_clear(&st);
    CHECK(compact_pool_add_pump(&st, NULL, &seed, &pid) == 0, "reseed buy_out");
    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.slot = 11;
    tx.tx_index = 2;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_BUY, 50, PUMP_DIR_QUOTE_TO_BASE);
    tx.ix[0].min_out = 0;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK, "exact-out buy");
    CHECK(st.pool[pid].pump.reserve_base == seed.reserve_base - 50,
          "buy_out moves exact base");
    CHECK(st.bank.version == 1, "buy_out published");
    CHECK(ar.cert.exactness_class == DEP_FAST_CUSTOM, "buy_out cert");

    /* unknown relevant Pump disc */
    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    leg(&tx.ix[0], pid, IX_KIND_OTHER, 50, PUMP_DIR_QUOTE_TO_BASE);
    tx.ix[0].proto = PROTO_PUMP;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_REJECT, "unknown pump kind reject");
    CHECK(ar.dep_class == DEP_UNKNOWN_PUMP_KIND, "classified UNKNOWN_PUMP_KIND");
    CHECK(st.bank.version == 1, "unknown is not identity");

    /* extra 0-account ix cannot write Pump quote/state */
    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    tx.slot = 12;
    tx.hdr_ok = 1;
    tx.nsig = 1;
    tx.n_static = 1;
    tx.n_keys = 1;
    tx.key[0][0] = 0x11;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_SELL, 10, PUMP_DIR_BASE_TO_QUOTE);
    tx.ix[1].proto = PROTO_UNKNOWN;
    tx.ix[1].kind = IX_KIND_OTHER;
    tx.ix[1].relevant = 1;
    tx.ix[1].prog = 0;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK, "0-acc extra is NO_QUOTE");
    CHECK(ar.dep_class == DEP_FAST_CUSTOM, "tx still FAST_CUSTOM");

    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    tx.slot = 12;
    tx.hdr_ok = 1;
    tx.nsig = 1;
    tx.n_static = 2;
    tx.n_keys = 2;
    tx.nro_signed = 0;
    tx.nro_unsigned = 1;
    memset(tx.key[1], 0x33, 32);
    tx.acc_n[1] = 2;
    tx.acc_ix[1][0] = 0;
    tx.acc_ix[1][1] = 1;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_SELL, 10, PUMP_DIR_BASE_TO_QUOTE);
    tx.ix[1].proto = PROTO_ATA;
    tx.ix[1].kind = IX_KIND_ATA_CREATE;
    tx.ix[1].relevant = 1;
    tx.ix[1].src_token = STATE_ACCT_NONE;
    tx.ix[1].dst_token = STATE_ACCT_NONE;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK, "ATA create extra ok");

    /* extra writable, quote/config deps not fully mapped → fail closed */
    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    tx.slot = 13;
    tx.hdr_ok = 1;
    tx.nsig = 1;
    tx.n_static = 2;
    tx.n_keys = 2;
    memset(tx.key[1], 0x22, 32);
    tx.acc_n[1] = 1;
    tx.acc_ix[1][0] = 1;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_SELL, 10, PUMP_DIR_BASE_TO_QUOTE);
    tx.ix[1].proto = PROTO_UNKNOWN;
    tx.ix[1].kind = IX_KIND_OTHER;
    tx.ix[1].relevant = 1;
    tx.ix[1].prog = 1;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_REJECT, "incomplete deps fail extra");
    CHECK(ar.dep_class == DEP_MISSING_CONFIG, "MISSING_CONFIG");

    /* armed vaults + zero fee bps: extra writable miss is NO_QUOTE */
    {
        pump_state_t armed = seed;
        uint8_t poolpk[32], vb[32], vq[32];

        armed.protocol_fee_bps = 0;
        armed.creator_fee_bps = 0;
        memset(poolpk, 0, 32);
        memset(vb, 0xaa, 32);
        memset(vq, 0xbb, 32);
        CHECK(compact_pool_arm(&st, poolpk, vb, vq, &armed, 1, &pid) == 0,
              "arm vaults");
    }
    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    tx.slot = 13;
    tx.hdr_ok = 1;
    tx.nsig = 1;
    tx.n_static = 2;
    tx.n_keys = 2;
    memset(tx.key[1], 0x22, 32);
    tx.acc_n[1] = 1;
    tx.acc_ix[1][0] = 1;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_SELL, 10, PUMP_DIR_BASE_TO_QUOTE);
    tx.ix[1].proto = PROTO_UNKNOWN;
    tx.ix[1].kind = IX_KIND_OTHER;
    tx.ix[1].relevant = 1;
    tx.ix[1].prog = 1;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK, "no-overlap extra is NO_QUOTE");

    /* extra writable ∩ pool identity → fail closed */
    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    tx.slot = 14;
    tx.hdr_ok = 1;
    tx.nsig = 1;
    tx.n_static = 1;
    tx.n_keys = 1;
    tx.acc_n[1] = 1;
    tx.acc_ix[1][0] = 0;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_SELL, 10, PUMP_DIR_BASE_TO_QUOTE);
    tx.ix[1].proto = PROTO_UNKNOWN;
    tx.ix[1].kind = IX_KIND_OTHER;
    tx.ix[1].relevant = 1;
    tx.ix[1].prog = 0;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_REJECT, "extra touches pool");
    CHECK(ar.dep_class == DEP_EXTRA_IX_TOUCHES_STATE, "EXTRA_IX_TOUCHES_STATE");

    /* exec failed */
    before = st;
    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.exec_failed = 1;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_BUY_EQ, 1000, PUMP_DIR_QUOTE_TO_BASE);
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_ABORTED, "exec_failed aborts");
    CHECK(memcmp(&st.pool[pid].pump, &before.pool[pid].pump,
                 sizeof(pump_state_t)) == 0,
          "abort does not publish");

    /* buy_exact_quote_in then sell on same pool */
    compact_state_clear(&st);
    CHECK(compact_pool_add_pump(&st, NULL, &seed, &pid) == 0, "reseed buy");
    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    leg(&tx.ix[0], pid, IX_KIND_PUMP_BUY_EQ, 5000, PUMP_DIR_QUOTE_TO_BASE);
    leg(&tx.ix[1], pid, IX_KIND_PUMP_SELL, 100, PUMP_DIR_BASE_TO_QUOTE);
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK, "buy_eq + sell overlay");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\noverlay ok\n");
    return 0;
}
