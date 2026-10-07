#include "cycle.h"
#include "live.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * PAPER-002 — quote one reconstructed DLMM+Pump pair.
 * Does not mutate frozen kernels. Prints hop intermediates.
 */
int
main(int argc, char **argv)
{
    live_univ_t u;
    uint32_t di = 0;
    uint32_t pi = 1;
    uint32_t i;
    opportunity_t sz;
    cycle_quote_t q;
    uint64_t winner_in = 0;
    const char *path;

    if (argc < 2) {
        fprintf(stderr, "usage: %s histuniv.bin [winner_amount_in]\n", argv[0]);
        return 1;
    }
    path = argv[1];
    if (argc >= 3) {
        winner_in = strtoull(argv[2], NULL, 10);
    }
    if (live_univ_init(&u) != 0 || live_univ_load(&u, path) != 0) {
        fprintf(stderr, "FAIL  load\n");
        return 1;
    }
    for (i = 0; i < u.n; i++) {
        if (u.meta[i].protocol == PROTO_DLMM) {
            di = i;
        }
        if (u.meta[i].protocol == PROTO_PUMP) {
            pi = i;
        }
    }
    {
        const dlmm_pool_hot_t *h = dlmm_cache_get(&u.dlmm, di);
        dlmm_state_t ds;
        const pump_state_t *ps = u.pump_live[pi] ? &u.pump[pi] : NULL;
        if (h == NULL || ps == NULL) {
            fprintf(stderr, "FAIL  pair dlmm=%u pump=%u\n", di, pi);
            live_univ_free(&u);
            return 1;
        }
        dlmm_hot_to_state(h, &ds);
        printf("{\n");
        printf("  \"n\": %u, \"dlmm_idx\": %u, \"pump_idx\": %u,\n", u.n, di, pi);
        printf("  \"slot\": %" PRIu64 ",\n", u.slot);
        if (cycle_size(&ds, ps, &sz) == 0 && sz.valid) {
            printf("  \"cycle_size\": {\"amount_in\": %" PRIu64
                   ", \"amount_out\": %" PRIu64 ", \"gross\": %" PRIu64
                   ", \"direction\": %u},\n",
                   sz.amount_in, sz.amount_out, sz.gross_profit, sz.direction);
        } else {
            printf("  \"cycle_size\": null,\n");
        }
        printf("  \"quotes\": [\n");
        for (i = 0; i < 2; i++) {
            uint64_t ain = winner_in ? winner_in : (sz.valid ? sz.amount_in : 0);
            if (ain == 0 || cycle_quote(&ds, ps, ain, (uint8_t)i, &q) != 0
                || !q.valid) {
                printf("    {\"direction\": %u, \"valid\": 0}%s\n",
                       i, i == 0 ? "," : "");
                continue;
            }
            printf("    {\"direction\": %u, \"valid\": 1, \"amount_in\": %" PRIu64
                   ", \"amount_out\": %" PRIu64 ", \"gross\": %" PRId64 "}%s\n",
                   i, q.amount_in, q.amount_out, q.gross_profit,
                   i == 0 ? "," : "");
        }
        printf("  ]\n}\n");
    }
    live_univ_free(&u);
    return 0;
}
