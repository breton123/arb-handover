#include "hot.h"
#include "live.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SOL  1000000000ull
#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", (m)); return 1; } \
} while (0)

int
main(int argc, char **argv)
{
    live_univ_t u;
    hot_trigger_t n;
    hot_decision_t d0;
    hot_decision_t d1;
    const char *path;
    uint64_t v0;
    uint64_t rx0;
    uint64_t rq0;
    const dlmm_pool_hot_t *h;

    if (argc != 2) {
        fprintf(stderr, "usage: %s liveuniv.bin\n", argv[0]);
        return 1;
    }
    path = argv[1];
    CHECK(live_univ_init(&u) == 0 && live_univ_load(&u, path) == 0, "load");
    v0 = u.state_version;
    CHECK(v0 != 0, "bootstrap version");
    h = dlmm_cache_get(&u.dlmm, 0);
    CHECK(h != NULL && u.pump_live[1], "canonical rows");
    rx0 = h->reserve_x;
    rq0 = u.pump[1].reserve_quote;

    memset(&n, 0, sizeof(n));
    n.protocol = PROTO_DLMM;
    n.dlmm.amount_in = SOL / 10ull;
    n.dlmm.min_amount_out = 0;
    n.dlmm.swap_for_y = 0;

    CHECK(hot_decide(&u, 0, &n, &d0) == 0, "eval on spec S'");
    CHECK(d0.state_version == v0, "decision stamped with current version");
    CHECK(!hot_stale(&u, d0.state_version), "fresh");
    CHECK(u.state_version == v0, "eval must not bump version");
    CHECK(dlmm_cache_get(&u.dlmm, 0)->reserve_x == rx0, "eval must not commit DLMM");
    CHECK(u.pump[1].reserve_quote == rq0, "eval must not commit Pump");
    printf("opportunity used state_version = %" PRIu64 "  valid=%u  gp=%" PRIu64 "\n",
           d0.state_version, d0.opp.valid, d0.opp.gross_profit);

    CHECK(hot_commit(&u, 0, &n) == 0, "confirmed apply");
    CHECK(u.state_version == v0 + 1, "commit bumps version");
    CHECK(hot_stale(&u, d0.state_version), "prior decision is stale");
    CHECK(dlmm_cache_get(&u.dlmm, 0)->reserve_x != rx0, "canonical DLMM moved");
    CHECK(u.pump[1].reserve_quote == rq0, "untouched Pump still canonical");

    CHECK(hot_decide(&u, 0, &n, &d1) == 0, "eval after commit");
    CHECK(d1.state_version == v0 + 1, "new decision uses new version");
    CHECK(!hot_stale(&u, d1.state_version), "new decision fresh");
    printf("after commit  state_version = %" PRIu64 "  valid=%u  gp=%" PRIu64 "\n",
           d1.state_version, d1.opp.valid, d1.opp.gross_profit);

    memset(&n, 0, sizeof(n));
    n.protocol = PROTO_PUMP;
    n.pump.amount_in = SOL / 10ull;
    n.pump.direction = PUMP_DIR_QUOTE_TO_BASE;
    CHECK(hot_commit(&u, 1, &n) == 0, "confirmed Pump apply");
    CHECK(u.state_version == v0 + 2, "second commit");
    CHECK(u.pump[1].reserve_quote != rq0, "canonical Pump moved");
    CHECK(hot_stale(&u, d1.state_version), "DLMM-era decision stale after Pump commit");

    live_univ_free(&u);
    printf("STATE-002  ok  spec != commit  version journalable\n");
    return 0;
}
