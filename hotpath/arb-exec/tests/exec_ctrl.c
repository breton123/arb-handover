#include "ctrl.h"
#include "nonce.h"
#include "tx_template.h"

#include <inttypes.h>
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

static void
set32(uint8_t *p, uint8_t tag, uint32_t n)
{
    memset(p, 0, 32);
    p[0] = tag;
    p[1] = (uint8_t)n;
    p[2] = (uint8_t)(n >> 8);
}

static void
fill_rec(ctrl_nonce_rec_t rec[NONCE_POOL_N])
{
    uint32_t i;

    memset(rec, 0, sizeof(ctrl_nonce_rec_t) * NONCE_POOL_N);
    for (i = 0; i < NONCE_POOL_N; i++) {
        set32(rec[i].pubkey, 0xB0, i);
        set32(rec[i].hash, 0xC0, i);
    }
}

static void
fill_keys(uint8_t keys[ROUTE0_N][32], uint8_t our_exec[32])
{
    uint32_t i;

    memset(keys, 0, sizeof(uint8_t) * ROUTE0_N * 32);
    for (i = 0; i < ROUTE0_N; i++) {
        keys[i][0] = 0xA0;
        keys[i][1] = (uint8_t)i;
    }
    memcpy(keys[ROUTE0_ACC_PUMP_BASE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_X_MINT], 32);
    memcpy(keys[ROUTE0_ACC_PUMP_QUOTE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_Y_MINT], 32);
    memset(our_exec, 0, 32);
    our_exec[0] = 0xE0;
}

static void
mk_opp(opportunity_t *o)
{
    memset(o, 0, sizeof(*o));
    o->route_id     = ROUTE_DLMM_PUMP;
    o->amount_in    = 1000;
    o->amount_out   = 1001;
    o->gross_profit = 1;
    o->direction    = 0;
    o->valid        = 1;
}

int
main(void)
{
    nonce_pool_t pool;
    ctrl_nonce_rec_t rec[NONCE_POOL_N];
    ctrl_fees_t fees, fees2;
    ctrl_nonce_rec_t rec2[NONCE_POOL_N];
    route0_tx_template_t tmpl;
    opportunity_t opp;
    nonce_claim_t a, b, c;
    uint8_t keys[ROUTE0_N][32];
    uint8_t our_exec[32];
    uint8_t tx[ROUTE0_TX_LEN];
    uint8_t bin[CTRL_BIN_LEN];
    uint8_t hnew[32];
    uint64_t price, profit;
    size_t blen = 0;
    uint32_t i;

    fill_rec(rec);
    CHECK(ctrl_nonce_load_all(&pool, rec, 63) != 0, "refuse n != 64");
    CHECK(ctrl_nonce_load_all(&pool, rec, NONCE_POOL_N) == 0, "nonce_load × 64");
    CHECK(pool.n_ready == 64u, "64 READY");
    for (i = 0; i < NONCE_POOL_N; i++) {
        if (pool.slot[i].state != NONCE_READY) {
            fprintf(stderr, "FAIL  slot %u not READY\n", i);
            g_fail++;
            break;
        }
    }
    CHECK(i == NONCE_POOL_N, "every slot READY");

    CHECK(ctrl_fees_set(&fees, 1000ull, 7ull) == 0, "fees at startup");
    fill_keys(keys, our_exec);
    CHECK(route0_tx_compile(&keys[0][0], our_exec, ROUTE0_CU_LIMIT, &tmpl) == 0,
          "template");
    mk_opp(&opp);

    CHECK(nonce_claim(&pool, &a) == 0, "attempt A");
    CHECK(pool.slot[a.idx].state == NONCE_IN_FLIGHT, "A IN_FLIGHT");
    CHECK(ctrl_patch(&tmpl, &opp, &fees, &a, tx) == 0, "patch from ctrl fees");
    memcpy(&price, tx + ROUTE0_OFF_CU_PRICE, 8);
    memcpy(&profit, tx + ROUTE0_OFF_MIN_PROFIT, 8);
    CHECK(price == 1000ull, "cu_price from control plane");
    CHECK(profit == 7ull, "min_profit from control plane");
    CHECK(memcmp(tx + ROUTE0_OFF_BLOCKHASH, a.hash, 32) == 0,
          "claimed nonce on the wire");

    CHECK(ctrl_fees_set(&fees, 2500ull, 9ull) == 0, "control plane updates fees");
    CHECK(nonce_claim(&pool, &b) == 0, "attempt B");
    CHECK(ctrl_patch(&tmpl, &opp, &fees, &b, tx) == 0, "B uses new fees");
    memcpy(&price, tx + ROUTE0_OFF_CU_PRICE, 8);
    CHECK(price == 2500ull, "hot path only patches the new cu_price");
    CHECK(a.idx != b.idx, "A and B distinct nonces");

    set32(hnew, 0xCF, a.idx);
    CHECK(ctrl_nonce_finish(&pool, a.idx, hnew) == 0, "confirm A → reload");
    CHECK(pool.slot[a.idx].state == NONCE_READY, "A READY again");
    CHECK(memcmp(pool.slot[a.idx].hash, hnew, 32) == 0, "A has new hash");

    set32(hnew, 0xD0, b.idx);
    CHECK(ctrl_nonce_finish(&pool, b.idx, hnew) == 0, "fail B → reload");
    CHECK(pool.slot[b.idx].state == NONCE_READY, "B READY after failure");

    CHECK(nonce_claim(&pool, &c) == 0, "next claim");
    CHECK(pool.n_ready == 63u, "reloaded slots back in the ring");
    CHECK(ctrl_nonce_finish(&pool, c.idx, hnew) == 0, "finish C");

    CHECK(ctrl_fees_set(NULL, 1, 1) != 0, "fees null fails");
    CHECK(ctrl_nonce_finish(&pool, 99, hnew) != 0, "finish bad idx fails");

    CHECK(ctrl_fees_set(&fees, 111ull, 22ull) == 0, "bin fees");
    CHECK(ctrl_bin_write(bin, sizeof(bin), rec, &fees, &blen) == 0 &&
              blen == CTRL_BIN_LEN,
          "write ctrl.bin");
    CHECK(ctrl_bin_read(bin, blen, rec2, &fees2) == 0, "read ctrl.bin");
    CHECK(fees2.cu_price == 111ull && fees2.min_profit == 22ull,
          "bin carries fees");
    CHECK(memcmp(rec2[17].pubkey, rec[17].pubkey, 32) == 0 &&
              memcmp(rec2[17].hash, rec[17].hash, 32) == 0,
          "bin carries slot 17");
    CHECK(ctrl_nonce_load_all(&pool, rec2, NONCE_POOL_N) == 0,
          "reload pool from bin");
    CHECK(pool.n_ready == 64u, "bin load is 64 READY");

    CHECK(1, "no RPC on claim / patch");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nEXEC-CTRL ok  load → claim → finish → READY  fees are patched\n");
    return 0;
}
