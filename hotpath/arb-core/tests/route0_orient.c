/*
 * Route0 denomination contract.
 * HTvjzsfX + Gf7sXMoP must not compile (shared USDC is not enough).
 */
#include "live.h"
#include "proto.h"
#include "universe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", (m)); return 1; } \
} while (0)

static const uint8_t TOK[32] = {
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18,
    0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90,
    0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78,
    0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0
};

static const uint8_t USDC[32] = {
    0xc6, 0xfa, 0x7a, 0xf3, 0xbe, 0xdb, 0xad, 0x3a,
    0x3d, 0x65, 0xf3, 0x6a, 0xab, 0xc9, 0x74, 0x31,
    0xb1, 0xbb, 0xe4, 0xc2, 0xd2, 0xf6, 0xe0, 0xe4,
    0x7c, 0xa6, 0x02, 0x03, 0x45, 0x2f, 0x5d, 0x61
};

static uint32_t
n_family(const universe_t *u, uint8_t fam)
{
    uint32_t i, n = 0;
    for (i = 0; i < u->n_route; i++) {
        if (u->route[i].family == fam) {
            n++;
        }
    }
    return n;
}

static uint32_t
n_route0(const universe_t *u)
{
    return n_family(u, ROUTE_FAM_0_DLMM_PUMP);
}

static int
has_pair(const universe_t *u, uint32_t a, uint32_t b)
{
    uint32_t i, h;
    for (i = 0; i < u->n_route; i++) {
        const compiled_route_t *r = &u->route[i];
        int saw_a = 0, saw_b = 0;
        if (r->family != ROUTE_FAM_0_DLMM_PUMP) {
            continue;
        }
        for (h = 0; h < r->n_hop; h++) {
            if (r->pool[h] == a) {
                saw_a = 1;
            }
            if (r->pool[h] == b) {
                saw_b = 1;
            }
        }
        if (saw_a && saw_b) {
            return 1;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    universe_t u;

    CHECK(route0_pair_compatible(TOK, MINT_SOL, TOK, MINT_SOL), "good pair");
    CHECK(!route0_pair_compatible(MINT_SOL, TOK, TOK, MINT_SOL),
          "inverted DLMM refused");
    CHECK(!route0_pair_compatible(TOK, MINT_SOL, MINT_SOL, TOK),
          "inverted Pump refused");
    CHECK(!route0_pair_compatible(USDC, MINT_SOL, TOK, MINT_SOL),
          "different token refused");
    CHECK(!route0_pair_compatible(MINT_SOL, MINT_SOL, TOK, MINT_SOL),
          "WSOL token refused");

    /* Correct orientation compiles. */
    CHECK(universe_init(&u, 8, 64) == 0, "init");
    CHECK(universe_add_pool(&u, PROTO_DLMM, TOK, MINT_SOL) == 0, "dlmm ok");
    CHECK(universe_add_pool(&u, PROTO_PUMP, TOK, MINT_SOL) == 0, "pump ok");
    CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "compile ok");
    CHECK(n_route0(&u) > 0, "route0 present");
    CHECK(has_pair(&u, 0, 1), "routes_by_pool contains the pair");
    universe_free(&u);

    /* HTvj shape: inverted DLMM (X=SOL Y=USDC) + Pump (base=USDC quote=SOL). */
    CHECK(universe_init(&u, 8, 64) == 0, "init ht");
    CHECK(universe_add_pool(&u, PROTO_DLMM, MINT_SOL, USDC) == 0, "HTvj-like");
    CHECK(universe_add_pool(&u, PROTO_PUMP, USDC, MINT_SOL) == 0, "Gf7s-like");
    CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "compile ht");
    CHECK(n_route0(&u) == 0, "HTvjzsfX+Gf7sXMoP route0 compile = REFUSED");
    CHECK(!has_pair(&u, 0, 1), "routes_by_pool[HTvj] does not contain that pair");
    CHECK(n_family(&u, ROUTE_FAM_5_DLMM_PUMP_TYPED) > 0,
          "inverted SOL cycle is typed family 5");
    universe_free(&u);

    /* Inverted TOKEN/WSOL DLMM + Pump TOKEN/WSOL: not route0, typed closed. */
    CHECK(universe_init(&u, 8, 64) == 0, "init inv");
    CHECK(universe_add_pool(&u, PROTO_DLMM, MINT_SOL, TOK) == 0, "dlmm inv");
    CHECK(universe_add_pool(&u, PROTO_PUMP, TOK, MINT_SOL) == 0, "pump tok");
    CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "compile inv");
    CHECK(n_route0(&u) == 0, "inverted TOKEN DLMM is not route0");
    CHECK(n_family(&u, ROUTE_FAM_5_DLMM_PUMP_TYPED) > 0,
          "inverted TOKEN DLMM typed");
    universe_free(&u);

    /* Same-mint share but Pump quote != WSOL. */
    CHECK(universe_init(&u, 8, 64) == 0, "init q");
    CHECK(universe_add_pool(&u, PROTO_DLMM, USDC, MINT_SOL) == 0, "dlmm usdc/sol");
    CHECK(universe_add_pool(&u, PROTO_PUMP, USDC, TOK) == 0, "pump usdc/token");
    CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "compile q");
    CHECK(n_route0(&u) == 0, "pump quote != WSOL refused");
    universe_free(&u);

    /* Supported closure: DLMM→DLMM is executable, never FAM_OTHER. */
    CHECK(universe_init(&u, 8, 64) == 0, "init d2");
    CHECK(universe_add_pool(&u, PROTO_DLMM, TOK, MINT_SOL) == 0, "d2a");
    CHECK(universe_add_pool(&u, PROTO_DLMM, TOK, MINT_SOL) == 0, "d2b");
    CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "compile d2");
    CHECK(n_family(&u, ROUTE_FAM_6_SUPPORTED) > 0, "dlmm-dlmm FAM_6");
    CHECK(n_family(&u, ROUTE_FAM_OTHER) == 0, "no 255 over DLMM-only");
    {
        uint32_t i;
        for (i = 0; i < u.n_route; i++) {
            CHECK(route_executable(&u.route[i]), "every compiled route executable");
        }
    }
    universe_free(&u);

    /* Pump→Pump same token, and 3-hop DLMM³, still supported. */
    CHECK(universe_init(&u, 8, 64) == 0, "init p2");
    CHECK(universe_add_pool(&u, PROTO_PUMP, TOK, MINT_SOL) == 0, "p2a");
    CHECK(universe_add_pool(&u, PROTO_PUMP, TOK, MINT_SOL) == 0, "p2b");
    CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "compile p2");
    CHECK(n_family(&u, ROUTE_FAM_6_SUPPORTED) > 0, "pump-pump FAM_6");
    CHECK(n_family(&u, ROUTE_FAM_OTHER) == 0, "no 255 over Pump-only");
    universe_free(&u);

    CHECK(universe_init(&u, 8, 64) == 0, "init d3");
    CHECK(universe_add_pool(&u, PROTO_DLMM, TOK, MINT_SOL) == 0, "d3a");
    CHECK(universe_add_pool(&u, PROTO_DLMM, TOK, USDC) == 0, "d3b");
    CHECK(universe_add_pool(&u, PROTO_DLMM, USDC, MINT_SOL) == 0, "d3c");
    CHECK(universe_compile(&u, UNIV_MASK_2) == 0, "compile d3");
    CHECK(n_family(&u, ROUTE_FAM_6_SUPPORTED) > 0, "dlmm3 FAM_6");
    {
        uint32_t i, hop3 = 0;
        for (i = 0; i < u.n_route; i++) {
            CHECK(route_executable(&u.route[i]), "dlmm3 executable");
            if (u.route[i].n_hop == 3) {
                hop3++;
            }
        }
        CHECK(hop3 > 0, "3-hop compiled");
    }
    universe_free(&u);

    if (argc == 2) {
        live_univ_t lu;
        uint32_t i, ht = UINT32_MAX, gp = UINT32_MAX;
        static const uint8_t HT[32] = {
            0xf4, 0xa0, 0xdb, 0x7d, 0xac, 0x99, 0xf6, 0xa0,
            0x5c, 0xbf, 0xf9, 0x6e, 0xd8, 0x01, 0xe0, 0x72,
            0x75, 0xb8, 0x2d, 0x6c, 0x0e, 0x4c, 0xd6, 0x67,
            0xed, 0x1f, 0x8b, 0xd8, 0x96, 0x6c, 0xc1, 0xd8
        };
        static const uint8_t GP[32] = {
            0xe8, 0xa3, 0x24, 0xd0, 0xc1, 0x5c, 0xf5, 0xea,
            0x66, 0x6a, 0x60, 0xa7, 0xaf, 0x69, 0x01, 0xfd,
            0xe6, 0x7c, 0x73, 0x5b, 0x29, 0x67, 0x52, 0x6d,
            0x09, 0x43, 0x34, 0xa4, 0xb2, 0x81, 0x23, 0xb9
        };
        CHECK(live_univ_init(&lu) == 0 && live_univ_load(&lu, argv[1]) == 0,
              "load liveuniv");
        CHECK(live_univ_assert_route0(&lu) == 0, "startup assert");
        for (i = 0; i < lu.n; i++) {
            if (memcmp(lu.meta[i].pubkey, HT, 32) == 0) {
                ht = i;
            }
            if (memcmp(lu.meta[i].pubkey, GP, 32) == 0) {
                gp = i;
            }
        }
        if (ht != UINT32_MAX && gp != UINT32_MAX) {
            CHECK(!has_pair(&lu.routes, ht, gp),
                  "live HTvjzsfX+Gf7sXMoP not in routes_by_pool");
            printf("ROUTE0-ORIENT  live HTvj idx=%u Gf7s idx=%u REFUSED\n",
                   ht, gp);
        }
        printf("ROUTE0-ORIENT  live n=%u route0=%u\n",
               lu.n, n_route0(&lu.routes));
        live_univ_free(&lu);
    }

    printf("ROUTE0-ORIENT  ok  denomination contract\n");
    return 0;
}
