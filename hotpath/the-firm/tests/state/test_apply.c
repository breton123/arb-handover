#include "deps/class.h"
#include "ingress/tx.h"
#include "state/compact.h"
#include "transitions/apply.h"

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

int
main(void)
{
    compact_state_t st;
    ordered_tx_t tx;
    apply_result_t ar;
    dep_report_t dep;
    pump_state_t seed;
    uint32_t pid = 0;

    memset(&seed, 0, sizeof(seed));
    seed.reserve_base = 1ull << 20;
    seed.reserve_quote = 1ull << 20;
    seed.lp_fee_bps = 20;
    compact_state_clear(&st);
    CHECK(compact_pool_add_pump(&st, NULL, &seed, &pid) == 0, "pool");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_SYSTEM;
    tx.ix[0].kind = IX_KIND_OTHER;
    tx.ix[0].relevant = 0;
    deps_classify(&tx, &dep);
    CHECK(dep.tx_class == DEP_IRRELEVANT, "irrelevant system");
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_IRRELEVANT, "apply irrelevant");
    CHECK(st.pool[pid].incarnation == 1, "pool incarnation untouched");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_DLMM;
    tx.ix[0].kind = IX_KIND_DLMM_SWAP;
    tx.ix[0].relevant = 1;
    tx.ix[0].pool_id = pid;
    deps_classify(&tx, &dep);
    CHECK(dep.tx_class == DEP_FALLBACK, "DLMM is fallback TODO");
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_REJECT, "fallback fail-closed");

    ordered_tx_clear(&tx);
    tx.slot = 99;
    tx.encoding = TX_ENC_V0;
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_PUMP;
    tx.ix[0].kind = IX_KIND_PUMP_BUY_EQ;
    tx.ix[0].relevant = 1;
    tx.ix[0].direction = PUMP_DIR_QUOTE_TO_BASE;
    tx.ix[0].pool_id = pid;
    tx.ix[0].amount_in = 10000;
    CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK, "v0 buy_exact_quote_in");
    CHECK(st.pool[pid].pump.reserve_base < seed.reserve_base, "base fell");
    CHECK(st.bank.slot == 99 && st.bank.version >= 1, "published identity");
    CHECK(pid == 0, "first pool id is 0");
    CHECK(STATE_ACCT_NONE != 0u, "NONE is not a valid state id");

    /* ID 0 is a real pool and vault. Unused fields must stay NONE. */
    {
        pump_state_t armed = seed;
        uint8_t poolpk[32], vb[32], vq[32];
        uint32_t tid_b = 99, tid_q = 99;
        uint64_t rb0, rq0;

        compact_state_clear(&st);
        memset(poolpk, 0x10, 32);
        memset(vb, 0xaa, 32);
        memset(vq, 0xbb, 32);
        armed.protocol_fee_bps = 0;
        armed.creator_fee_bps = 0;
        CHECK(compact_pool_arm(&st, poolpk, vb, vq, &armed, 1, &pid) == 0,
              "arm pool id 0");
        CHECK(pid == 0, "armed pool is id 0");
        CHECK(compact_token_find(&st, vb, &tid_b) == 0 && tid_b == 0,
              "base vault is token id 0");
        CHECK(compact_token_find(&st, vq, &tid_q) == 0 && tid_q == 1,
              "quote vault is token id 1");
        rb0 = st.pool[0].pump.reserve_base;
        rq0 = st.pool[0].pump.reserve_quote;

        ordered_tx_clear(&tx);
        tx.slot = 50;
        tx.encoding = TX_ENC_V0;
        tx.n_ix = 3;
        tx.ix[0].proto = PROTO_ATA;
        tx.ix[0].kind = IX_KIND_ATA_CREATE;
        tx.ix[0].relevant = 1;
        tx.ix[0].pool_id = STATE_ACCT_NONE;
        tx.ix[0].src_token = STATE_ACCT_NONE;
        tx.ix[0].dst_token = STATE_ACCT_NONE;
        tx.ix[0].src_sys = STATE_ACCT_NONE;
        tx.ix[0].dst_sys = STATE_ACCT_NONE;
        tx.ix[1].proto = PROTO_PUMP;
        tx.ix[1].kind = IX_KIND_PUMP_SELL;
        tx.ix[1].relevant = 1;
        tx.ix[1].direction = PUMP_DIR_BASE_TO_QUOTE;
        tx.ix[1].pool_id = 0;
        tx.ix[1].src_token = STATE_ACCT_NONE;
        tx.ix[1].dst_token = STATE_ACCT_NONE;
        tx.ix[1].vault_base = 0;
        tx.ix[1].vault_quote = 1;
        tx.ix[1].fee_proto = STATE_ACCT_NONE;
        tx.ix[1].fee_creator = STATE_ACCT_NONE;
        tx.ix[1].src_sys = STATE_ACCT_NONE;
        tx.ix[1].dst_sys = STATE_ACCT_NONE;
        tx.ix[1].amount_in = 100;
        tx.ix[1].min_out = 0;
        tx.ix[2].proto = PROTO_TOKEN;
        tx.ix[2].kind = IX_KIND_TOKEN_CLOSE;
        tx.ix[2].relevant = 1;
        tx.ix[2].pool_id = STATE_ACCT_NONE;
        tx.ix[2].src_token = STATE_ACCT_NONE;
        tx.ix[2].dst_token = STATE_ACCT_NONE;
        tx.ix[2].src_sys = STATE_ACCT_NONE;
        tx.ix[2].dst_sys = STATE_ACCT_NONE;
        CHECK(apply_tx(&st, &tx, &ar) == APPLY_OK,
              "id0 vault + ATA/CLOSE NONE applies");
        CHECK(st.pool[0].pump.reserve_base > rb0
                  && st.pool[0].pump.reserve_quote < rq0,
              "id0 pool advanced");
        CHECK(st.token[0].amount == st.pool[0].pump.reserve_base,
              "id0 vault follows pump");

        ordered_tx_clear(&tx);
        tx.slot = 51;
        tx.encoding = TX_ENC_V0;
        tx.n_ix = 1;
        tx.ix[0].proto = PROTO_TOKEN;
        tx.ix[0].kind = IX_KIND_TOKEN_CLOSE;
        tx.ix[0].relevant = 1;
        tx.ix[0].src_token = 0; /* real vault */
        tx.ix[0].dst_token = STATE_ACCT_NONE;
        tx.ix[0].dst_sys = STATE_ACCT_NONE;
        CHECK(apply_tx(&st, &tx, &ar) == APPLY_REJECT,
              "close of mapped id0 vault without dest rejects");
        CHECK(ar.fail_step == APPLY_STEP_TOKEN, "reject is TOKEN_IX");
        CHECK(ar.fail_ix == 0, "reject ix 0");
    }

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\napply ok\n");
    return 0;
}
