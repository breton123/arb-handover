#include "dlmm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static dlmm_state_t
base_state(void)
{
    dlmm_state_t s;

    memset(&s, 0, sizeof(s));
    s.active_id = 0;
    s.bin_step = 100;
    s.now_ts = 1;
    s.parameters.base_factor = 0;
    s.parameters.filter_period = 10;
    s.parameters.decay_period = 120;
    s.parameters.reduction_factor = 5000;
    s.parameters.max_volatility_accumulator = 100000;
    s.parameters.protocol_share = 0;
    s.v_parameters.last_update_timestamp = 0;
    s.nbin = 2;
    s.bins[0].id = 0;
    s.bins[0].amount_y = 1000000;
    s.bins[0].amount_x = 0;
    s.bins[1].id = -1;
    s.bins[1].amount_y = 500000;
    s.bins[1].amount_x = 0;
    s.reserve_y = 1500000;
    return s;
}

static int
check_price_zero(void)
{
    unsigned __int128 p;
    if (dlmm_price_from_id(0, 100, &p) != 0) {
        fprintf(stderr, "price(0) fail\n");
        return -1;
    }
    if (p != ((unsigned __int128)1 << 64)) {
        fprintf(stderr, "price(0) != 1.0 Q64\n");
        return -1;
    }
    return 0;
}

static int
check_partial_bin(void)
{
    dlmm_state_t before = base_state();
    dlmm_state_t after;
    dlmm_swap_ix_t ix = { .amount_in = 1000, .min_amount_out = 0, .swap_for_y = 1 };
    dlmm_apply_result_t res;

    if (dlmm_apply_swap(&before, &ix, &after, &res) != 0) {
        fprintf(stderr, "partial apply failed\n");
        return -1;
    }
    if (res.amount_in != 1000 || res.amount_out != 1000) {
        fprintf(stderr, "partial io %" PRIu64 " / %" PRIu64 "\n",
                res.amount_in, res.amount_out);
        return -1;
    }
    if (after.active_id != 0 || after.bins[0].amount_x != 1000
        || after.bins[0].amount_y != 999000) {
        fprintf(stderr, "partial bin state wrong\n");
        return -1;
    }
    return 0;
}

static int
check_cross_bin(void)
{
    dlmm_state_t before = base_state();
    dlmm_state_t after;
    dlmm_swap_ix_t ix = { .amount_in = 1000000 + 10, .min_amount_out = 0, .swap_for_y = 1 };
    dlmm_apply_result_t res;

    before.bins[0].amount_y = 1000000;
    if (dlmm_apply_swap(&before, &ix, &after, &res) != 0) {
        fprintf(stderr, "cross apply failed\n");
        return -1;
    }
    if (after.bins[0].amount_y != 0) {
        fprintf(stderr, "first bin not drained\n");
        return -1;
    }
    if (after.active_id != -1 && after.bins[0].amount_y == 0 && res.amount_in < ix.amount_in) {
        fprintf(stderr, "did not walk\n");
        return -1;
    }
    if (res.amount_out == 0) {
        fprintf(stderr, "zero out\n");
        return -1;
    }
    return 0;
}

static int
check_min_out(void)
{
    dlmm_state_t before = base_state();
    dlmm_state_t after;
    dlmm_swap_ix_t ix = { .amount_in = 1000, .min_amount_out = 100000, .swap_for_y = 1 };
    dlmm_apply_result_t res;

    if (dlmm_apply_swap(&before, &ix, &after, &res) == 0) {
        fprintf(stderr, "min_out should fail\n");
        return -1;
    }
    return 0;
}

static int
check_swap2_disc(void)
{
    uint8_t raw[24] = {
        65, 75, 63, 76, 235, 91, 91, 136,
        232, 3, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    dlmm_swap_ix_t ix;
    if (dlmm_decode_swap2(raw, 24, &ix) != 0 || ix.amount_in != 1000) {
        fprintf(stderr, "swap2 decode\n");
        return -1;
    }
    return 0;
}

int
main(void)
{
    int fails = 0;

    fails += check_price_zero() != 0;
    fails += check_partial_bin() != 0;
    fails += check_cross_bin() != 0;
    fails += check_min_out() != 0;
    fails += check_swap2_disc() != 0;
    if (fails) {
        printf("CORE-003  FAIL  %d\n", fails);
        return 1;
    }
    printf("CORE-003\n");
    printf("  price(id=0)              ok\n");
    printf("  exact-in same-bin        ok  1000 → 1000 @ Q64.64 1.0\n");
    printf("  exact-in cross-bin       ok\n");
    printf("  min_amount_out reject    ok\n");
    printf("  swap2 disc               ok\n");
    printf("\nrestricted: SPL exact-in, no LO, no Token-2022\n");
    printf("live before/after triples: not in this dump (need account recorder)\n");
    return 0;
}
