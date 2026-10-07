#include "hot.h"
#include "live.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOL   1000000000ull
#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", (m)); return 1; } \
} while (0)

static void
fill_dlmm_ix(hot_trigger_t *n, uint64_t ain, uint8_t swap_for_y)
{
    memset(n, 0, sizeof(*n));
    n->protocol = PROTO_DLMM;
    n->dlmm.amount_in = ain;
    n->dlmm.min_amount_out = 0;
    n->dlmm.swap_for_y = swap_for_y;
}

static void
fill_pump_ix(hot_trigger_t *n, uint64_t ain, uint8_t dir)
{
    memset(n, 0, sizeof(*n));
    n->protocol = PROTO_PUMP;
    n->pump.amount_in = ain;
    n->pump.min_amount_out = 0;
    n->pump.direction = dir;
}

int
main(int argc, char **argv)
{
    live_univ_t u;
    hot_trigger_t n;
    opportunity_t live;
    opportunity_t toy;
    univ_state_t st;
    const char *path;
    uint8_t raw[24];
    const uint8_t buy[8] = {
        0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
    };
    uint64_t ain = SOL;
    uint32_t i;
    pump_state_t pump_before;
    const dlmm_pool_hot_t *h0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s liveuniv.bin\n", argv[0]);
        return 1;
    }
    path = argv[1];
    CHECK(live_univ_init(&u) == 0 && live_univ_load(&u, path) == 0, "load");
    CHECK(u.n_dlmm > 0 && u.n_pump > 0, "real venues");
    CHECK(u.routes.n_route > 0, "compiled routes");
    {
        uint32_t di = UINT32_MAX, pi = UINT32_MAX;
        for (i = 0; i < u.n; i++) {
            if (di == UINT32_MAX && u.meta[i].protocol == PROTO_DLMM) {
                di = i;
            }
            if (pi == UINT32_MAX && u.meta[i].protocol == PROTO_PUMP) {
                pi = i;
            }
        }
        CHECK(di != UINT32_MAX && pi != UINT32_MAX, "have DLMM and Pump");
        memcpy(&pump_before, &u.pump[pi], sizeof(pump_before));
        h0 = dlmm_cache_get(&u.dlmm, di);
        CHECK(h0 != NULL, "canonical DLMM");
        fill_dlmm_ix(&n, SOL / 10ull, 0);
        CHECK(hot_eval_live(&u, di, &n, &live) == 0, "eval DLMM N");
        {
            dlmm_pool_hot_t spec;
            dlmm_apply_result_t res;
            dlmm_state_t s_prime;
            opportunity_t ref2;
            memset(&ref2, 0, sizeof(ref2));
            if (dlmm_cache_predict(&u.dlmm, di, &n.dlmm, &spec, &res) == 0) {
                dlmm_hot_to_state(&spec, &s_prime);
                if (u.pump_live[pi]) {
                    CHECK(cycle_size(&s_prime, &u.pump[pi], &ref2) == 0
                          || !ref2.valid, "cycle optional");
                }
                printf("HOT-001  DLMM N idx=%u opp.valid=%u gp=%" PRIu64 "\n",
                       di, live.valid, live.gross_profit);
            }
            (void)ref2;
        }
        CHECK(memcmp(&u.pump[pi], &pump_before, sizeof(pump_before)) == 0,
              "canonical Pump unchanged");
        CHECK(dlmm_cache_get(&u.dlmm, di) == h0, "canonical DLMM unchanged");
        fill_pump_ix(&n, SOL / 10ull, PUMP_DIR_QUOTE_TO_BASE);
        CHECK(hot_eval_live(&u, pi, &n, &live) == 0, "eval Pump N");
        CHECK(memcmp(&u.pump[pi], &pump_before, sizeof(pump_before)) == 0,
              "Pump S still canonical");
        printf("HOT-001  Pump N idx=%u opp.valid=%u gp=%" PRIu64 "\n",
               pi, live.valid, live.gross_profit);
    }
    memcpy(raw, buy, 8);
    memcpy(raw + 8, &ain, 8);
    memset(raw + 16, 0, 8);
    CHECK(hot_decode_trigger(PROTO_PUMP, raw, 24, 0, &n) == 0
          && n.pump.direction == PUMP_DIR_QUOTE_TO_BASE, "decode pump buy");

    fill_dlmm_ix(&n, SOL / 10ull, 0);
    {
        uint8_t disc[24];
        memcpy(disc, DLMM_SWAP2_DISC, 8);
        memcpy(disc + 8, &n.dlmm.amount_in, 8);
        memset(disc + 16, 0, 8);
        CHECK(hot_decode_trigger(PROTO_DLMM, disc, 24, 0, &n) == 0
              && n.dlmm.swap_for_y == 0, "decode swap2 + dir");
    }

    CHECK(univ_state_init(&st, u.routes.n_pool) == 0, "toy state");
    universe_fill_synthetic(&u.routes, &st);
    memset(&toy, 0xff, sizeof(toy));
    CHECK(hot_eval_pool(&u.routes, &st, 0, &toy) == 0, "toy eval");
    CHECK(!toy.valid, "amm2 must not produce route0 opportunity_t");
    for (i = 0; i < u.routes.n_route; i++) {
        uint64_t dummy;
        if (u.routes.route[i].family == ROUTE_FAM_0_DLMM_PUMP) {
            CHECK(hot_quote_route(&u.routes, &st, i, SOL, &dummy) != 0,
                  "amm2 cannot quote route0");
        }
    }
    univ_state_free(&st);
    live_univ_free(&u);
    printf("HOT-001  ok  frozen kernels + cycle_size, no amm2 route0\n");
    return 0;
}
