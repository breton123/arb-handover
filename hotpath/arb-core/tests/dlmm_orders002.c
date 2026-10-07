#include "dlmm_orders.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", m); return 1; } } while (0)

static const unsigned __int128 Q1 = ((unsigned __int128)1) << 64;

static void
zero_state(dlmm_o_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->status = 0;
    s->bin_step = 1;
    s->nbin = 1;
    s->bins[0].id = 0;
    s->bins[0].price = Q1;
    s->function_type = DLMM_FN_LIMIT_ORDER;
    s->reward_mint_live_mask = 0;
    s->now_ts = 1;
    s->v_parameters.last_update_timestamp = 1;
}

static int
swap_y(const dlmm_o_state_t *s, uint64_t ain, dlmm_o_state_t *after, dlmm_apply_result_t *res)
{
    dlmm_swap_ix_t ix;
    ix.amount_in = ain;
    ix.min_amount_out = 0;
    ix.swap_for_y = 1;
    return dlmm_o_apply_swap(s, &ix, after, res);
}

int
main(void)
{
    dlmm_o_state_t s, after;
    dlmm_apply_result_t res;
    dlmm_o_fill_t f;

    /* MM sufficient — orders untouched */
    zero_state(&s);
    s.bins[0].amount_y = 100;
    s.bins[0].open_order_amount = 50;
    s.bins[0].processed_order_remaining_amount = 40;
    s.reserve_y = 190;
    CHECK(swap_y(&s, 20, &after, &res) == 0, "mm ok");
    CHECK(res.amount_out == 20, "mm quote");
    CHECK(after.bins[0].amount_y == 80, "mm debit");
    CHECK(after.bins[0].open_order_amount == 50, "open held");
    CHECK(after.bins[0].processed_order_remaining_amount == 40, "proc held");

    /* MM exhausted → processed */
    zero_state(&s);
    s.bins[0].amount_y = 20;
    s.bins[0].processed_order_remaining_amount = 10;
    s.bins[0].open_order_amount = 30;
    s.reserve_y = 60;
    CHECK(swap_y(&s, 25, &after, &res) == 0, "mm+proc ok");
    CHECK(res.amount_out == 25, "mm+proc quote");
    CHECK(after.bins[0].amount_y == 0, "mm empty");
    CHECK(after.bins[0].processed_order_remaining_amount == 5, "proc partial");
    CHECK(after.bins[0].open_order_amount == 30, "open held");

    /* processed exhausted → open */
    zero_state(&s);
    s.bins[0].amount_y = 20;
    s.bins[0].processed_order_remaining_amount = 10;
    s.bins[0].open_order_amount = 30;
    s.reserve_y = 60;
    CHECK(swap_y(&s, 35, &after, &res) == 0, "to open ok");
    CHECK(res.amount_out == 35, "to open quote");
    CHECK(after.bins[0].amount_y == 0, "mm gone");
    CHECK(after.bins[0].processed_order_remaining_amount == 0, "proc gone");
    CHECK(after.bins[0].open_order_amount == 25, "open debit");

    /* all three consumed */
    zero_state(&s);
    s.bins[0].amount_y = 20;
    s.bins[0].processed_order_remaining_amount = 10;
    s.bins[0].open_order_amount = 30;
    s.reserve_y = 60;
    CHECK(swap_y(&s, 60, &after, &res) == 0, "all ok");
    CHECK(res.amount_out == 60, "all quote");
    CHECK(after.bins[0].amount_y == 0, "all mm");
    CHECK(after.bins[0].processed_order_remaining_amount == 0, "all proc");
    CHECK(after.bins[0].open_order_amount == 0, "all open");
    CHECK(after.bins[0].amount_x == 20, "only mm in credited");

    /* wrong-side open ignored */
    zero_state(&s);
    s.bins[0].amount_y = 5;
    s.bins[0].open_order_amount = 10000;
    s.bins[0].limit_order_ask_side = 1;
    s.reserve_y = 5;
    CHECK(swap_y(&s, 5, &after, &res) == 0, "wrong side ok");
    CHECK(res.amount_out == 5, "wrong side quote");
    CHECK(after.bins[0].open_order_amount == 10000, "ask ignored");
    CHECK(swap_y(&s, 6, &after, &res) != 0, "cannot take ask as bid");

    /* function_type = LM disables orders */
    zero_state(&s);
    s.function_type = DLMM_FN_LIQUIDITY_MINING;
    s.bins[0].amount_y = 5;
    s.bins[0].open_order_amount = 100;
    s.reserve_y = 105;
    CHECK(swap_y(&s, 5, &after, &res) == 0, "lm ok");
    CHECK(after.bins[0].open_order_amount == 100, "lm open held");
    CHECK(swap_y(&s, 6, &after, &res) != 0, "lm no hidden depth");

    /* function_type = 0 + live reward → disabled; empty rewards → enabled */
    zero_state(&s);
    s.function_type = DLMM_FN_UNDETERMINED;
    s.reward_mint_live_mask = 1;
    s.bins[0].amount_y = 1;
    s.bins[0].open_order_amount = 50;
    s.reserve_y = 51;
    CHECK(swap_y(&s, 2, &after, &res) != 0, "ft0+reward no orders");
    s.reward_mint_live_mask = 0;
    CHECK(swap_y(&s, 2, &after, &res) == 0, "ft0 empty rewards");
    CHECK(after.bins[0].open_order_amount == 49, "ft0 used open");

    /* two bins */
    zero_state(&s);
    s.nbin = 2;
    s.active_id = 1;
    s.bins[0].id = 0;
    s.bins[0].price = Q1;
    s.bins[0].amount_y = 0;
    s.bins[0].open_order_amount = 7;
    s.bins[1].id = 1;
    s.bins[1].price = Q1;
    s.bins[1].amount_y = 3;
    s.bins[1].open_order_amount = 4;
    s.reserve_y = 14;
    CHECK(swap_y(&s, 10, &after, &res) == 0, "2bin ok");
    CHECK(res.amount_out == 10, "2bin quote");
    CHECK(after.bins[1].amount_y == 0 && after.bins[1].open_order_amount == 0, "bin1 empty");
    CHECK(after.bins[0].open_order_amount == 4, "bin0 open left");
    CHECK(after.active_id == 0, "walked");

    /* fill_bin layer split */
    zero_state(&s);
    s.bins[0].amount_y = 20;
    s.bins[0].processed_order_remaining_amount = 10;
    s.bins[0].open_order_amount = 30;
    CHECK(dlmm_o_fill_bin(&s.bins[0], 60, 1, 1, &f) == 0, "fill_bin");
    CHECK(f.mm_out == 20 && f.proc_out == 10 && f.open_out == 30, "layers");

    printf("dlmm_orders002 ok\n");
    return 0;
}
