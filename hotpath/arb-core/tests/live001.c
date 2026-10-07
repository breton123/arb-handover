#include "live.h"
#include "proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SOL  1000000000ull
#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", (m)); return 1; } \
} while (0)

static void
pk8(const uint8_t k[32], char *out)
{
    snprintf(out, 18, "%02x%02x%02x%02x..", k[0], k[1], k[2], k[3]);
}

int
main(int argc, char **argv)
{
    live_univ_t u;
    const char *path;
    uint32_t i;
    uint32_t q_dlmm = 0;
    uint32_t q_pump = 0;
    uint32_t fail_dlmm = 0;
    uint32_t fail_pump = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s liveuniv.bin\n", argv[0]);
        return 1;
    }
    path = argv[1];
    CHECK(live_univ_init(&u) == 0, "init");
    CHECK(live_univ_load(&u, path) == 0, "load real snapshot");
    CHECK(u.n > 0, "pools");
    CHECK(u.n_dlmm > 0, "at least one real DLMM");
    CHECK(u.n_pump > 0, "at least one real Pump");
    CHECK(u.state_version != 0, "state_version");
    CHECK(u.routes.n_pool == u.n, "universe is the snapshot");

    printf("LIVE-001  slot=%" PRIu64 "  ver=%" PRIu64 "  pools=%u  dlmm=%u  pump=%u  routes=%u\n",
           u.slot, u.state_version, u.n, u.n_dlmm, u.n_pump, u.routes.n_route);

    for (i = 0; i < u.n; i++) {
        char pk[20];
        const live_meta_t *m = &u.meta[i];
        const char *sol = memcmp(m->mint_y, MINT_SOL, 32) == 0 ? "Y=SOL"
            : memcmp(m->mint_x, MINT_SOL, 32) == 0 ? "X=SOL" : "no-SOL";
        pk8(m->pubkey, pk);
        printf("  [%u] proto=%u  %s  routes=%u  %s\n",
               i, m->protocol, pk, u.routes.pool[i].route_n, sol);

        if (m->protocol == PROTO_DLMM) {
            dlmm_quote_t q;
            if (live_dlmm_quote(&u, i, SOL / 100ull, 1, &q) == 0 && q.valid) {
                q_dlmm++;
                printf("       dlmm_quote 0.01 SOL→X  out=%" PRIu64 "\n", q.amount_out);
            } else if (live_dlmm_quote(&u, i, SOL / 100ull, 0, &q) == 0 && q.valid) {
                q_dlmm++;
                printf("       dlmm_quote 0.01 X→Y    out=%" PRIu64 "\n", q.amount_out);
            } else {
                fail_dlmm++;
                printf("       dlmm_quote fail-closed (missing bin or empty)\n");
            }
        } else if (m->protocol == PROTO_PUMP) {
            pump_quote_t q;
            if (live_pump_quote(&u, i, SOL / 100ull, PUMP_DIR_QUOTE_TO_BASE, &q) == 0
                && q.valid) {
                q_pump++;
                printf("       pump_quote 0.01 SOL→base  out=%" PRIu64 "\n", q.amount_out);
            } else if (live_pump_quote(&u, i, 1000000ull, PUMP_DIR_BASE_TO_QUOTE, &q) == 0
                       && q.valid) {
                q_pump++;
                printf("       pump_quote 1e6 base→SOL   out=%" PRIu64 "\n", q.amount_out);
            } else {
                fail_pump++;
                printf("       pump_quote fail-closed\n");
            }
        }
    }

    printf("quoted  dlmm_ok=%u  dlmm_fail=%u  pump_ok=%u  pump_fail=%u\n",
           q_dlmm, fail_dlmm, q_pump, fail_pump);
    CHECK(q_dlmm > 0, "real S → dlmm_quote");
    CHECK(q_pump > 0, "real S → pump_quote");
    live_univ_free(&u);
    printf("LIVE-001  ok  (frozen kernels, real accounts, no seed universe)\n");
    return 0;
}
