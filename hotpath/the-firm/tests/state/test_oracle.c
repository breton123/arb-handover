#include "oracle/oracle.h"

#include <stdio.h>
#include <stdlib.h>
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
    compact_state_t *pre, *post;
    ordered_tx_t tx;
    apply_result_t ar;
    oracle_diff_t d;
    pump_state_t seed;
    uint32_t pid = 0;

    pre = calloc(1, sizeof(*pre));
    post = calloc(1, sizeof(*post));
    if (pre == NULL || post == NULL) {
        return 1;
    }

    memset(&seed, 0, sizeof(seed));
    seed.reserve_base = 1ull << 20;
    seed.reserve_quote = 1ull << 20;
    seed.lp_fee_bps = 20;
    compact_state_clear(pre);
    CHECK(compact_pool_add_pump(pre, NULL, &seed, &pid) == 0, "pre pool");

    ordered_tx_clear(&tx);
    tx.slot = 7;
    tx.tx_index = 3;
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_PUMP;
    tx.ix[0].kind = IX_KIND_PUMP_SELL;
    tx.ix[0].relevant = 1;
    tx.ix[0].direction = PUMP_DIR_BASE_TO_QUOTE;
    tx.ix[0].pool_id = pid;
    tx.ix[0].amount_in = 1000;
    *post = *pre;
    CHECK(apply_tx(post, &tx, &ar) == APPLY_OK, "certified post");
    CHECK(oracle_replay(pre, &tx, post, DEP_FAST_CUSTOM, &d) == ORACLE_EXACT,
          "FAST_CUSTOM exact");
    CHECK(d.class_ours == DEP_FAST_CUSTOM && d.class_expect == DEP_FAST_CUSTOM,
          "class labels");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_SYSTEM;
    tx.ix[0].kind = IX_KIND_OTHER;
    tx.ix[0].relevant = 0;
    *post = *pre;
    CHECK(oracle_replay(pre, &tx, post, DEP_IRRELEVANT, &d) == ORACLE_EXACT,
          "IRRELEVANT proven");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_DLMM;
    tx.ix[0].kind = IX_KIND_DLMM_SWAP;
    tx.ix[0].relevant = 1;
    tx.ix[0].pool_id = pid;
    CHECK(oracle_replay(pre, &tx, post, DEP_FALLBACK, &d) == ORACLE_EXACT,
          "FALLBACK required");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_PUMP;
    tx.ix[0].kind = IX_KIND_OTHER;
    tx.ix[0].relevant = 1;
    tx.ix[0].pool_id = pid;
    CHECK(oracle_replay(pre, &tx, post, DEP_UNKNOWN, &d) == ORACLE_EXACT,
          "UNKNOWN");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        free(pre);
        free(post);
        return 1;
    }
    printf("\noracle ok\n");
    free(pre);
    free(post);
    return 0;
}
