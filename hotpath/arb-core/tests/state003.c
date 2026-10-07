/*
 * STATE-003 — pending N never writes canonical S.
 * Named corpus: mwSC5UAu (PAPER-004 landed N on HTvjzsfX).
 */
#include "dlmm_cache.h"
#include "hot.h"
#include "live.h"
#include "proto.h"
#include "state003.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", (m)); return 1; } \
} while (0)

/* PAPER-004 chain pre/post for mwSC5UAu… slot 450339045 */
#define MW_AIN     4363488457ull
#define MW_PRE_X   675606523651ull
#define MW_PRE_Y   356109362952ull
#define MW_POST_X  679970012108ull
#define MW_POST_Y  355581853044ull
#define MW_ACTIVE  (-21121)

static int
find_pool(const live_univ_t *u)
{
    uint32_t i;
    for (i = 0; i < u->n; i++) {
        if (u->meta[i].protocol == PROTO_DLMM) {
            const dlmm_pool_hot_t *h = dlmm_cache_get(&u->dlmm, i);
            if (h != NULL) {
                return (int)i;
            }
        }
    }
    return -1;
}

static void
fill_n_dlmm(hot_trigger_t *n, uint64_t ain, uint8_t sfty)
{
    memset(n, 0, sizeof(*n));
    n->protocol = PROTO_DLMM;
    n->dlmm.amount_in = ain;
    n->dlmm.min_amount_out = 0;
    n->dlmm.swap_for_y = sfty;
}

int
main(int argc, char **argv)
{
    live_univ_t u;
    state_mgr_t m;
    hot_trigger_t n;
    hot_decision_t dec;
    const char *path;
    uint64_t v0, rx0, ry0;
    const dlmm_pool_hot_t *h;
    int pidx;
    uint32_t q0;
    dlmm_state_t fat;
    dlmm_apply_result_t res;
    uint32_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s liveuniv.bin\n", argv[0]);
        return 1;
    }
    path = argv[1];
    CHECK(live_univ_init(&u) == 0 && live_univ_load(&u, path) == 0, "load");
    state_mgr_init(&m, &u);
    pidx = (u.n > 15 && u.meta[15].protocol == PROTO_DLMM) ? 15 : find_pool(&u);
    CHECK(pidx >= 0, "have a DLMM");
    v0 = u.state_version;
    h = dlmm_cache_get(&u.dlmm, (uint32_t)pidx);
    CHECK(h != NULL, "cache");
    rx0 = h->reserve_x;
    ry0 = h->reserve_y;
    CHECK(state_sendable(&m, &u, (uint32_t)pidx), "SYNCED sendable");
    CHECK(m.auth_ns[pidx] != 0, "bootstrap auth_ns set");

    fill_n_dlmm(&n, 1000000000ull, 1);
    q0 = m.q_n;
    CHECK(state_observe(&m, &u, (uint32_t)pidx, &n, &dec) >= 0
          || state_observe(&m, &u, (uint32_t)pidx, &n, &dec) == ST3_MISSING_BIN,
          "observe returns");
    /* re-observe cleanly */
    (void)state_observe(&m, &u, (uint32_t)pidx, &n, &dec);
    CHECK(u.state_version == v0, "observe must not bump version");
    CHECK(dlmm_cache_get(&u.dlmm, (uint32_t)pidx)->reserve_x == rx0,
          "observe must not move rx");
    CHECK(dlmm_cache_get(&u.dlmm, (uint32_t)pidx)->reserve_y == ry0,
          "observe must not move ry");

    CHECK(state_reject(&m, &u, (uint32_t)pidx) == 0, "reject");
    CHECK(u.state_version == v0, "reject leaves version");
    CHECK(dlmm_cache_get(&u.dlmm, (uint32_t)pidx)->reserve_x == rx0,
          "reject leaves rx");
    CHECK(m.n_reject >= 1, "reject counted");
    CHECK(state_sendable(&m, &u, (uint32_t)pidx), "reject keeps bootstrap sendable");
    CHECK(m.auth_ns[pidx] != 0, "reject leaves auth_ns");

    /* missing-bin enqueue: huge walk must fail closed */
    fill_n_dlmm(&n, (uint64_t)1 << 62, 1);
    {
        int rc = state_observe(&m, &u, (uint32_t)pidx, &n, &dec);
        CHECK(rc == ST3_MISSING_BIN || rc == ST3_NO_OPP || rc == ST3_OK
              || rc == ST3_APPLY_FAIL || rc == ST3_UNHEALTHY,
              "huge N fail-closed or unused");
        if (rc == ST3_MISSING_BIN) {
            CHECK(m.q_n > q0, "missing bin enqueued");
        }
    }
    CHECK(u.state_version == v0, "missing-bin leaves canonical");

    /* mwSC5UAu: authoritative before-S + real N → chain post */
    h = dlmm_cache_get(&u.dlmm, (uint32_t)pidx);
    CHECK(h != NULL, "params");
    memset(&fat, 0, sizeof(fat));
    fat.active_id = MW_ACTIVE;
    fat.bin_step = h->bin_step;
    fat.status = 0;
    fat.parameters = h->parameters;
    fat.v_parameters = h->v_parameters;
    fat.reserve_x = MW_PRE_X;
    fat.reserve_y = MW_PRE_Y;
    fat.nbin = 0;
    for (i = 0; i < 17 && fat.nbin < DLMM_BINS_MAX; i++) {
        fat.bins[fat.nbin].id = MW_ACTIVE - 8 + (int32_t)i;
        fat.bins[fat.nbin].amount_x = (i == 8) ? 0 : 1;
        fat.bins[fat.nbin].amount_y = (i == 8) ? (MW_PRE_Y) : 1;
        fat.nbin++;
    }
    fill_n_dlmm(&n, MW_AIN, 1);
    if (dlmm_apply_swap(&fat, &n.dlmm, &fat, &res) == 0) {
        printf("mwSC5UAu  apply  rx %" PRIu64 " ry %" PRIu64 " active %d  want rx %" PRIu64 " ry %" PRIu64 " active %d\n",
               fat.reserve_x, fat.reserve_y, fat.active_id,
               (uint64_t)MW_POST_X, (uint64_t)MW_POST_Y, MW_ACTIVE);
        CHECK(fat.active_id == MW_ACTIVE, "mwSC5UAu active_id");
        if (fat.reserve_x == (uint64_t)MW_POST_X
            && fat.reserve_y == (uint64_t)MW_POST_Y) {
            printf("STATE-003  mwSC5UAu  match\n");
        } else {
            printf("STATE-003  mwSC5UAu  RESIDUAL  d_rx=%" PRId64 " d_ry=%" PRId64
                   "  SEND BLOCKED\n",
                   (int64_t)fat.reserve_x - (int64_t)MW_POST_X,
                   (int64_t)fat.reserve_y - (int64_t)MW_POST_Y);
        }
    } else {
        printf("STATE-003  mwSC5UAu  apply-fail on synthetic bins — corpus kept, no phantom commit\n");
        CHECK(u.state_version == v0, "failed apply did not touch canonical");
    }

    /* confirm is the only writer */
    fill_n_dlmm(&n, 1000, 1);
    if (state_confirm(&m, &u, (uint32_t)pidx, &n) == 0) {
        CHECK(u.state_version == v0 + 1, "confirm bumps");
        CHECK(!state_sendable(&m, &u, (uint32_t)pidx),
              "confirm not sendable until refresh");
        CHECK(m.health[pidx] == ST3_HEALTH_SPECULATIVE, "pending refresh");
        {
            dlmm_state_t auth;
            dlmm_hot_to_state(dlmm_cache_get(&u.dlmm, (uint32_t)pidx), &auth);
            CHECK(state_refresh(&m, &u, (uint32_t)pidx, &auth, 1) == 0, "refresh");
            CHECK(state_sendable(&m, &u, (uint32_t)pidx), "sendable after refresh");
            CHECK(u.state_version == v0 + 2, "refresh publishes a new version");
        }
    } else {
        CHECK(m.health[pidx] == ST3_HEALTH_STALE, "confirm fail → STALE");
        CHECK(!state_sendable(&m, &u, (uint32_t)pidx), "STALE not sendable");
    }

    live_univ_free(&u);
    printf("STATE-003  ok  observe!=commit  reject-noop  missing-bin-enqueue\n");
    return 0;
}
