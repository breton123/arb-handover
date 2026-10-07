/*
 * STATE-004 — DLMM reserve_* is priced liquidity (bin-sum), not the SPL vault.
 * mwSC5UAu: fee excluded from reserve_x equals the vault residual.
 */
#include "dlmm.h"
#include "dlmm_cache.h"
#include "hot.h"
#include "live.h"
#include "proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", (m)); return 1; } \
} while (0)

#define MW_AIN     4363488457ull
#define MW_PRE_X   675606523651ull
#define MW_PRE_Y   356109362952ull
#define MW_POST_X  679970012108ull
#define MW_POST_Y  355581853044ull
#define MW_ACTIVE  (-21121)

int
main(int argc, char **argv)
{
    live_univ_t u;
    const dlmm_pool_hot_t *h;
    dlmm_state_t fat, after;
    dlmm_apply_result_t res;
    hot_trigger_t n;
    uint64_t bin_sum_x, bin_sum_y;
    uint16_t i;
    int pidx;

    if (argc != 2) {
        fprintf(stderr, "usage: %s liveuniv.bin\n", argv[0]);
        return 1;
    }
    CHECK(live_univ_init(&u) == 0 && live_univ_load(&u, argv[1]) == 0, "load");
    pidx = (u.n > 15 && u.meta[15].protocol == PROTO_DLMM) ? 15 : -1;
    CHECK(pidx >= 0, "pool 15");
    h = dlmm_cache_get(&u.dlmm, (uint32_t)pidx);
    CHECK(h != NULL, "cache");

    memset(&fat, 0, sizeof(fat));
    fat.active_id = MW_ACTIVE;
    fat.bin_step = h->bin_step;
    fat.parameters = h->parameters;
    fat.v_parameters = h->v_parameters;
    fat.reserve_x = MW_PRE_X;
    fat.reserve_y = MW_PRE_Y;
    fat.nbin = 17;
    for (i = 0; i < 17; i++) {
        fat.bins[i].id = MW_ACTIVE - 8 + (int32_t)i;
        fat.bins[i].amount_x = (i == 8) ? 0 : 1;
        fat.bins[i].amount_y = (i == 8) ? MW_PRE_Y : 1;
    }
    memset(&n, 0, sizeof(n));
    n.protocol = PROTO_DLMM;
    n.dlmm.amount_in = MW_AIN;
    n.dlmm.swap_for_y = 1;
    CHECK(dlmm_apply_swap(&fat, &n.dlmm, &after, &res) == 0, "apply");

    bin_sum_x = 0;
    bin_sum_y = 0;
    for (i = 0; i < after.nbin; i++) {
        bin_sum_x += after.bins[i].amount_x;
        bin_sum_y += after.bins[i].amount_y;
    }

    printf("STATE-004  mwSC5UAu\n");
    printf("  active_id          %d  (chain %d)\n", after.active_id, MW_ACTIVE);
    printf("  amount_in          %" PRIu64 "\n", res.amount_in);
    printf("  amount_out         %" PRIu64 "\n", res.amount_out);
    printf("  total_fee          %" PRIu64 "\n", res.fee);
    printf("  protocol_fee       %" PRIu64 "\n", res.protocol_fee);
    printf("  lp_fee             %" PRIu64 "\n", res.fee - res.protocol_fee);
    printf("  kernel_rx          %" PRIu64 "\n", after.reserve_x);
    printf("  kernel_ry          %" PRIu64 "\n", after.reserve_y);
    printf("  vault_rx           %" PRIu64 "\n", (uint64_t)MW_POST_X);
    printf("  vault_ry           %" PRIu64 "\n", (uint64_t)MW_POST_Y);
    printf("  vault_x - kernel_x %" PRId64 "\n",
           (int64_t)MW_POST_X - (int64_t)after.reserve_x);
    printf("  vault_y - kernel_y %" PRId64 "\n",
           (int64_t)MW_POST_Y - (int64_t)after.reserve_y);
    printf("  kernel_rx - bin_sum_x %" PRId64 "  (synthetic bins, not a chain proof)\n",
           (int64_t)after.reserve_x - (int64_t)bin_sum_x);
    {
        uint64_t pre_plus_in = (uint64_t)MW_PRE_X + (uint64_t)MW_AIN;
        uint64_t priced = (uint64_t)MW_PRE_X + res.amount_in - res.fee;
        uint64_t priced_plus_fee = after.reserve_x + res.fee;
        printf("  identity vault_x ?= pre_x + ain : %s  (%" PRIu64 " vs %" PRIu64 ")\n",
               (pre_plus_in == (uint64_t)MW_POST_X) ? "YES" : "NO",
               pre_plus_in, (uint64_t)MW_POST_X);
        printf("  identity kernel_rx ?= pre_x + ain - fee : %s  (%" PRIu64 " vs %" PRIu64 ")\n",
               (priced == after.reserve_x) ? "YES" : "NO",
               priced, after.reserve_x);
        printf("  identity vault_x ?= kernel_rx + fee : %s  (%" PRIu64 " vs %" PRIu64 ")\n",
               (priced_plus_fee == (uint64_t)MW_POST_X) ? "YES" : "NO",
               priced_plus_fee, (uint64_t)MW_POST_X);
        CHECK(after.active_id == MW_ACTIVE, "active exact");
        CHECK(pre_plus_in == (uint64_t)MW_POST_X, "vault_x = pre + user_in");
        CHECK(priced == after.reserve_x, "kernel_rx = pre + in - fee");
        CHECK(priced_plus_fee == (uint64_t)MW_POST_X,
              "ACCOUNT: vault_x = priced_rx + total_fee");
    }

    printf("PRICE  active_id exact\n");
    printf("ACCOUNT  vault_x residual == total_fee (%" PRIu64 ")  explained\n", res.fee);
    printf("ACCOUNT  vault_y residual %" PRId64 "  (need chain Y-out vs kernel amount_out)\n",
           (int64_t)MW_POST_Y - (int64_t)after.reserve_y);
    live_univ_free(&u);
    return 0;
}
