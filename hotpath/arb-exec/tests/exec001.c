#include "route0.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define START_QUOTE  1000000ull
#define START_BASE   0ull
#define RESERVE      100000000ull
#define AMOUNT_IN    1000ull

static int g_fail;

#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            fprintf(stderr, "FAIL  %s\n", (msg));          \
            g_fail++;                                      \
        } else {                                           \
            printf("ok    %s\n", (msg));                   \
        }                                                  \
    } while (0)

static void
set_key(uint8_t *k, uint8_t tag)
{
    memset(k, 0, 32);
    k[0] = tag;
}

static void
mk_token(exec_account_t *a, uint8_t tag, uint8_t mint, uint64_t amt, int writable)
{
    memset(a, 0, sizeof(*a));
    set_key(a->key, tag);
    set_key(a->owner, 3); /* token program */
    a->dlen     = SPL_TOKEN_LEN;
    a->writable = writable ? 1 : 0;
    set_key(a->data + SPL_MINT_OFF, mint);
    set_key(a->data + SPL_OWNER_OFF, 1); /* authority */
    memcpy(a->data + SPL_AMOUNT_OFF, &amt, 8);
}

static void
mk_prog(exec_account_t *a, uint8_t tag)
{
    memset(a, 0, sizeof(*a));
    set_key(a->key, tag);
}

static void
mk_cfg(exec_account_t *a, uint8_t tag, uint64_t num, uint64_t den, uint8_t fail)
{
    memset(a, 0, sizeof(*a));
    set_key(a->key, tag);
    a->dlen     = 17;
    a->writable = 1;
    memcpy(a->data, &num, 8);
    memcpy(a->data + 8, &den, 8);
    a->data[16] = fail;
}

static void
world(exec_account_t *acc, uint64_t dlmm_num, uint64_t dlmm_den,
      uint64_t pump_num, uint64_t pump_den, uint8_t pump_fail)
{
    uint32_t i;

    memset(acc, 0, sizeof(exec_account_t) * ROUTE0_N);
    for (i = 0; i < ROUTE0_N; i++) {
        mk_prog(&acc[i], (uint8_t)(i + 1));
    }

    set_key(acc[ROUTE0_ACC_AUTHORITY].key, 1);
    acc[ROUTE0_ACC_AUTHORITY].signer    = 1;
    acc[ROUTE0_ACC_AUTHORITY].writable  = 1;
    acc[ROUTE0_ACC_AUTHORITY].lamports  = 1;

    mk_token(&acc[ROUTE0_ACC_USER_QUOTE], 2, 16, START_QUOTE, 1);
    mk_token(&acc[ROUTE0_ACC_USER_BASE], 3, 15, START_BASE, 1);
    mk_cfg(&acc[ROUTE0_ACC_DLMM_LB_PAIR], 10, dlmm_num, dlmm_den, 0);
    mk_token(&acc[ROUTE0_ACC_DLMM_RESERVE_X], 12, 15, RESERVE, 1);
    mk_token(&acc[ROUTE0_ACC_DLMM_RESERVE_Y], 13, 16, RESERVE, 1);
    acc[ROUTE0_ACC_DLMM_BIN_0].writable = 1;
    acc[ROUTE0_ACC_DLMM_BIN_1].writable = 1;
    acc[ROUTE0_ACC_DLMM_ORACLE].writable = 1;
    mk_cfg(&acc[ROUTE0_ACC_PUMP_POOL], 22, pump_num, pump_den, pump_fail);
    mk_token(&acc[ROUTE0_ACC_PUMP_POOL_BASE], 28, 15, RESERVE, 1);
    mk_token(&acc[ROUTE0_ACC_PUMP_POOL_QUOTE], 29, 16, RESERVE, 1);
    mk_token(&acc[ROUTE0_ACC_PUMP_PROTO_FEE_ATA], 31, 16, 0, 1);
    mk_token(&acc[ROUTE0_ACC_PUMP_CREATOR_ATA], 32, 16, 0, 1);
    acc[ROUTE0_ACC_PUMP_USER_VOL].writable = 1;
}

static int
same_world(const exec_account_t *a, const exec_account_t *b)
{
    return memcmp(a, b, sizeof(exec_account_t) * ROUTE0_N) == 0;
}

static uint64_t
amt(const exec_account_t *a)
{
    uint64_t v = 0;
    memcpy(&v, a->data + SPL_AMOUNT_OFF, 8);
    return v;
}

static int
run_opp(exec_account_t *acc, uint8_t dir, uint64_t amount_in,
        uint64_t min_profit, route0_result_t *r)
{
    opportunity_t opp;
    uint8_t ix[ROUTE0_IX_LEN];
    size_t n = 0;
    uint64_t packed_in;

    memset(&opp, 0, sizeof(opp));
    opp.route_id     = ROUTE_DLMM_PUMP;
    opp.amount_in    = amount_in;
    opp.amount_out   = 0;
    opp.gross_profit = 0;
    opp.direction    = dir;
    opp.valid        = 1;

    if (route0_pack(&opp, min_profit, ix, sizeof(ix), &n) != ROUTE0_OK) {
        return -1;
    }
    if (n != ROUTE0_IX_LEN) {
        return -1;
    }
    memcpy(&packed_in, ix + 9, 8);
    if (packed_in != amount_in || ix[8] != dir) {
        return -1;
    }
    return route0_process(acc, ROUTE0_N, ix, n, r);
}

int
main(void)
{
    exec_account_t acc[ROUTE0_N], before[ROUTE0_N];
    route0_result_t r;
    opportunity_t bad;
    uint8_t ix[ROUTE0_IX_LEN];
    size_t n;
    uint32_t i;
    uint64_t packed_in;

    printf("EXEC-001  route 0  accounts=%u\n", ROUTE0_N);

    CHECK(ROUTE0_N == 35, "frozen account count");
    CHECK(strcmp(route0_acc_name[0], "authority") == 0, "acc[0] authority");
    CHECK(strcmp(route0_acc_name[1], "user_quote") == 0, "acc[1] user_quote");
    CHECK(strcmp(route0_acc_name[2], "user_base") == 0, "acc[2] user_base");
    CHECK(strcmp(route0_acc_name[7], "dlmm_program") == 0, "acc[7] dlmm_program");
    CHECK(strcmp(route0_acc_name[19], "pump_program") == 0, "acc[19] pump_program");
    CHECK(strcmp(route0_acc_name[34], "pump_user_vol") == 0, "acc[34] last");
    for (i = 0; i < ROUTE0_N; i++) {
        if (route0_acc_name[i] == NULL || route0_acc_name[i][0] == '\0') {
            fprintf(stderr, "FAIL  unnamed account %u\n", i);
            g_fail++;
        }
    }
    if (g_fail == 0) {
        printf("ok    every account named\n");
    }

    /* amount_in from opportunity_t */
    {
        opportunity_t opp;
        memset(&opp, 0, sizeof(opp));
        opp.route_id  = ROUTE_DLMM_PUMP;
        opp.amount_in = 424242ull;
        opp.direction = 1;
        opp.valid     = 1;
        n = 0;
        CHECK(route0_pack(&opp, 7, ix, sizeof(ix), &n) == ROUTE0_OK,
              "pack opportunity_t");
        memcpy(&packed_in, ix + 9, 8);
        CHECK(packed_in == 424242ull, "amount_in comes from opportunity_t");
        CHECK(ix[8] == 1, "direction comes from opportunity_t");
    }

    memset(&bad, 0, sizeof(bad));
    CHECK(route0_pack(&bad, 1, ix, sizeof(ix), &n) == ROUTE0_ERR_IX,
          "reject invalid opportunity");

    /* DLMM → Pump atomic success: 1:1 then 2x sell */
    world(acc, 1, 1, 2, 1, 0);
    memcpy(before, acc, sizeof(before));
    CHECK(run_opp(acc, ROUTE0_DIR_DLMM_THEN_PUMP, AMOUNT_IN, 1, &r) == ROUTE0_OK,
          "DLMM→Pump executes");
    CHECK(r.rc == ROUTE0_OK, "DLMM→Pump rc");
    CHECK(amt(&acc[ROUTE0_ACC_USER_QUOTE]) == START_QUOTE + AMOUNT_IN,
          "DLMM→Pump profit returns to inventory");
    CHECK(amt(&acc[ROUTE0_ACC_USER_BASE]) == START_BASE,
          "DLMM→Pump base inventory flat");
    CHECK(r.quote_after == START_QUOTE + AMOUNT_IN, "quote_after recorded");
    CHECK(r.cu > 0, "CU recorded (DLMM→Pump)");
    printf("      CU  %" PRIu64 "\n", r.cu);

    /* Pump → DLMM atomic success: 2x buy then 1:1 back */
    world(acc, 1, 1, 2, 1, 0);
    CHECK(run_opp(acc, ROUTE0_DIR_PUMP_THEN_DLMM, AMOUNT_IN, 1, &r) == ROUTE0_OK,
          "Pump→DLMM executes");
    CHECK(amt(&acc[ROUTE0_ACC_USER_QUOTE]) == START_QUOTE + AMOUNT_IN,
          "Pump→DLMM profit returns to inventory");
    CHECK(amt(&acc[ROUTE0_ACC_USER_BASE]) == START_BASE,
          "Pump→DLMM base inventory flat");
    CHECK(r.cu > 0, "CU recorded (Pump→DLMM)");
    printf("      CU  %" PRIu64 "\n", r.cu);

    /* profit guard */
    world(acc, 1, 1, 2, 1, 0);
    memcpy(before, acc, sizeof(before));
    CHECK(run_opp(acc, ROUTE0_DIR_DLMM_THEN_PUMP, AMOUNT_IN, AMOUNT_IN + 1, &r)
              == ROUTE0_ERR_PROFIT,
          "profit guard rejects tight min_profit");
    CHECK(same_world(acc, before), "profit reject leaves no residue");

    /* stale / unprofitable (1:2 dump) */
    world(acc, 1, 1, 1, 2, 0);
    memcpy(before, acc, sizeof(before));
    CHECK(run_opp(acc, ROUTE0_DIR_DLMM_THEN_PUMP, AMOUNT_IN, 1, &r)
              == ROUTE0_ERR_PROFIT,
          "stale/unprofitable route reverts");
    CHECK(same_world(acc, before), "unprofitable revert is atomic");

    /* second-leg fail: Pump disabled after DLMM would have swapped */
    world(acc, 1, 1, 2, 1, 1);
    memcpy(before, acc, sizeof(before));
    CHECK(run_opp(acc, ROUTE0_DIR_DLMM_THEN_PUMP, AMOUNT_IN, 1, &r)
              == ROUTE0_ERR_PUMP,
          "Pump fail after DLMM");
    CHECK(same_world(acc, before), "failure cannot leave us half-swapped");

    world(acc, 1, 1, 2, 1, 1);
    memcpy(before, acc, sizeof(before));
    CHECK(run_opp(acc, ROUTE0_DIR_PUMP_THEN_DLMM, AMOUNT_IN, 1, &r)
              == ROUTE0_ERR_PUMP,
          "Pump fail before DLMM");
    CHECK(same_world(acc, before), "first-leg fail is a no-op");

    /* signer required */
    world(acc, 1, 1, 2, 1, 0);
    acc[ROUTE0_ACC_AUTHORITY].signer = 0;
    {
        opportunity_t opp;
        memset(&opp, 0, sizeof(opp));
        opp.route_id  = ROUTE_DLMM_PUMP;
        opp.amount_in = AMOUNT_IN;
        opp.direction = 0;
        opp.valid     = 1;
        n = 0;
        route0_pack(&opp, 1, ix, sizeof(ix), &n);
        CHECK(route0_process(acc, ROUTE0_N, ix, n, &r) == ROUTE0_ERR_SIGNER,
              "missing signer rejected");
    }

    /* wrong account count */
    world(acc, 1, 1, 2, 1, 0);
    {
        opportunity_t opp;
        memset(&opp, 0, sizeof(opp));
        opp.route_id  = ROUTE_DLMM_PUMP;
        opp.amount_in = AMOUNT_IN;
        opp.valid     = 1;
        n = 0;
        route0_pack(&opp, 1, ix, sizeof(ix), &n);
        CHECK(route0_process(acc, ROUTE0_N - 1, ix, n, &r) == ROUTE0_ERR_ACCOUNTS,
              "wrong account count rejected");
    }

    printf("\naccount layout\n");
    for (i = 0; i < ROUTE0_N; i++) {
        printf("  %2u  %s\n", i, route0_acc_name[i]);
    }

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nEXEC-001 ok  route-0 layout frozen\n");
    return 0;
}
