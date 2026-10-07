#include "adapter.h"
#include "classify.h"
#include "classify_n.h"
#include "hot.h"
#include "proto.h"
#include "tsc.h"
#include "tx_n.h"
#include "universe.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", m); return 1; } \
} while (0)

#define WARM  200u
#define ITER  2000u

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t
pct(uint64_t *v, uint32_t n, double p)
{
    uint32_t i;
    if (n == 0) {
        return 0;
    }
    qsort(v, n, sizeof(*v), cmp_u64);
    i = (uint32_t)(p * (double)(n - 1u));
    return v[i];
}

static void
mint(uint8_t o[32], uint8_t tag)
{
    memset(o, 0xa5, 32);
    o[31] = tag;
}

static int
bench_eval(universe_t *u, univ_state_t *st, uint32_t pool, const char *label)
{
    struct tsc_clock clk;
    uint64_t *samp;
    uint32_t i;
    opportunity_t opp;
    volatile uint32_t sink = 0;

    if (tsc_calibrate(&clk) != 0) {
        return -1;
    }
    samp = calloc(ITER, sizeof(*samp));
    if (samp == NULL) {
        return -1;
    }
    for (i = 0; i < WARM; i++) {
        (void)hot_eval_pool(u, st, pool, &opp);
        sink ^= opp.valid;
    }
    for (i = 0; i < ITER; i++) {
        uint64_t t0 = rdtscp();
        (void)hot_eval_pool(u, st, pool, &opp);
        samp[i] = tsc_to_ns(&clk, rdtscp() - t0);
        sink ^= opp.valid;
    }
    printf("%-16s  pools=%-5u routes=%-8u  p50=%5" PRIu64 " ns  p99=%5" PRIu64
           " ns  p999=%5" PRIu64 " ns\n",
           label, u->n_pool, u->n_route,
           pct(samp, ITER, 0.50), pct(samp, ITER, 0.99), pct(samp, ITER, 0.999));
    free(samp);
    (void)sink;
    return 0;
}

int
main(void)
{
    uint8_t buf[256];
    uint8_t tx[32];
    uint8_t ty[32];
    universe_t u;
    univ_state_t st;
    opportunity_t opp;
    amm2_state_t a, ap;
    clmm_state_t c;
    uint64_t out;
    uint32_t rel2, rel6;
    size_t off;
    uint8_t proto;

    memset(buf, 0x11, sizeof(buf));
    memcpy(buf + 40, PROG_DLMM, 32);
    rel2 = classify_n(buf, sizeof(buf), 2);
    rel6 = classify_n(buf, sizeof(buf), 6);
    CHECK((rel2 & REL_N_DLMM) != 0, "classify_n(2) finds DLMM");
    CHECK((rel6 & REL_N_DLMM) != 0, "classify_n(6) finds DLMM");
    CHECK(classify_avx2(buf, (uint16_t)sizeof(buf)) & REL_DLMM,
          "locked 2-ID still finds DLMM");
    CHECK(find_prog_id_n(buf, sizeof(buf), 6, &off, &proto) == 0
          && proto == PROTO_DLMM && off == 40, "find_prog_id_n");

    memcpy(buf + 80, PROG_CLMM, 32);
    CHECK(classify_n(buf, sizeof(buf), 2) == REL_N_DLMM,
          "2-ID does not see CLMM");
    CHECK((classify_n(buf, sizeof(buf), 3) & REL_N_CLMM) != 0,
          "3-ID sees CLMM");

    a.reserve_x = 100ull * 1000000000ull;
    a.reserve_y = 200ull * 1000000000ull;
    a.fee_bps = 25;
    a.status = 1;
    CHECK(amm2_quote(&a, 1000000000ull, 0, &out) == ADAPTER_OK && out > 0,
          "amm2 quote");
    CHECK(amm2_apply(&a, 1000000000ull, 0, &ap, &out) == ADAPTER_OK
          && ap.reserve_x > a.reserve_x, "amm2 apply");

    memset(&c, 0, sizeof(c));
    CHECK(clmm_quote(&c, 1000, 0, &out) == ADAPTER_NO_LIQ,
          "CLMM fail-closed without ticks");
    c.status = 1;
    c.has_ticks = 1;
    c.liquidity = 1ull << 40;
    c.sqrt_price_x64 = 1ull << 63;
    c.fee_bps = 25;
    CHECK(clmm_quote(&c, 1000000ull, 0, &out) == ADAPTER_OK, "CLMM in-range");

    mint(tx, 1);
    mint(ty, 2);
    CHECK(universe_init(&u, 64, 4096) == 0, "univ init");
    CHECK(universe_add_pool(&u, PROTO_DLMM, MINT_SOL, tx) == 0, "dlmm pool");
    CHECK(universe_add_pool(&u, PROTO_PUMP, MINT_SOL, tx) == 0, "pump pool");
    CHECK(universe_add_pool(&u, PROTO_CLMM, MINT_SOL, tx) == 0, "clmm pool");
    CHECK(universe_compile(&u, UNIV_MASK_3) == 0, "compile 2/3 hop");
    CHECK(u.n_route >= 6, "2-hop pairs compiled");
    CHECK(u.pool[0].route_n > 0, "routes_by_pool[0]");
    CHECK(univ_state_init(&st, u.n_pool) == 0, "state");
    universe_fill_synthetic(&u, &st);
    CHECK(hot_apply_pool(&u, &st, 0, 10000000ull, 0) != 0,
          "amm2 must not apply DLMM");
    CHECK(hot_eval_pool(&u, &st, 0, &opp) == 0, "eval");
    CHECK(!opp.valid, "amm2 must not emit route0 opportunity");
    universe_free(&u);
    univ_state_free(&st);

    CHECK(universe_init(&u, 256, 65536) == 0, "seed init");
    CHECK(universe_seed(&u, 6) == 0, "seed 6");
    CHECK(universe_compile(&u, UNIV_MASK_6) == 0, "compile 6");
    printf("CORE-009  seed6  pools=%u  routes=%u\n", u.n_pool, u.n_route);
    CHECK(u.n_route > 0, "seed routes");
    {
        uint32_t i;
        uint32_t hop2 = 0, hop3 = 0;
        for (i = 0; i < u.n_route; i++) {
            if (u.route[i].n_hop == 2) {
                hop2++;
            } else if (u.route[i].n_hop == 3) {
                hop3++;
            }
            CHECK(u.route[i].n_hop <= 3, "V1 cap 3 hop");
        }
        printf("          hop2=%u  hop3=%u\n", hop2, hop3);
        CHECK(hop2 > 0 && hop3 > 0, "both 2-hop and 3-hop");
    }
    universe_free(&u);

    printf("\nUniverse scale  (hot_eval_pool[0] only)\n");
    {
        const uint32_t ns[] = { 10000u, 100000u, 1000000u };
        uint32_t k;
        for (k = 0; k < 3; k++) {
            char lab[32];
            CHECK(universe_init(&u, 8, ns[k] + 64u) == 0, "scale init");
            CHECK(universe_add_pool(&u, PROTO_DLMM, MINT_SOL, tx) == 0, "s0");
            CHECK(universe_add_pool(&u, PROTO_PUMP, MINT_SOL, tx) == 0, "s1");
            CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "scale compile");
            CHECK(universe_scale_dummy(&u, ns[k]) == 0, "dummy");
            CHECK(univ_state_init(&st, u.n_pool) == 0, "scale st");
            universe_fill_synthetic(&u, &st);
            snprintf(lab, sizeof(lab), "%u routes", ns[k]);
            CHECK(bench_eval(&u, &st, 0, lab) == 0, "bench");
            universe_free(&u);
            univ_state_free(&st);
        }
    }

    printf("CORE-009  ok\n");
    return 0;
}
