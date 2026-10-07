#ifndef ARB_CORE_DLMM_ORDERS_H
#define ARB_CORE_DLMM_ORDERS_H

/*
 * DLMM-ORDERS-002 — offline STATE-011 / order-aware kernel contract.
 *
 * Not linked into `core`. Do not include from live.c / paper.c / meteora_dlmm.c
 * until STATE-010 soak is done and this lands as one generation.
 *
 * On-chain Bin is one inventory + one side flag, not two open books:
 *   open_order_amount + limit_order_ask_side
 * Bid vs ask is a direction gate, not two stored reserves.
 */

#include "dlmm.h"

#include <stdint.h>

#define DLMM_FN_UNDETERMINED     0u
#define DLMM_FN_LIQUIDITY_MINING 1u
#define DLMM_FN_LIMIT_ORDER      2u

typedef struct {
    int32_t  id;
    uint64_t amount_x;
    uint64_t amount_y;
    uint64_t processed_order_remaining_amount;
    uint64_t open_order_amount;
    uint64_t total_processing_order_amount; /* bookkeeping; not a fillable layer */
    uint32_t order_age;
    uint8_t  limit_order_ask_side; /* 0 bid, !=0 ask */
    unsigned __int128 price;       /* 0 → compute from id + bin_step */
} dlmm_o_bin_t;

typedef struct {
    int32_t              active_id;
    uint16_t             bin_step;
    uint8_t              status;
    uint8_t              function_type;
    uint8_t              reward_mint_live_mask; /* bit i set ⇒ reward_infos[i].mint != default */
    dlmm_static_params_t parameters;
    dlmm_vparams_t       v_parameters;
    uint64_t             reserve_x;
    uint64_t             reserve_y;
    dlmm_o_bin_t         bins[DLMM_BINS_MAX];
    uint16_t             nbin;
    int64_t              now_ts;
} dlmm_o_state_t;

typedef struct {
    uint64_t mm_in;
    uint64_t mm_out;
    uint64_t proc_in;
    uint64_t proc_out;
    uint64_t open_in;
    uint64_t open_out;
    uint64_t used_in; /* excluded-fee input consumed across layers */
    uint64_t left;    /* excluded-fee input remaining */
    uint64_t out_amt; /* output produced across layers */
} dlmm_o_fill_t;

int dlmm_o_orders_enabled(const dlmm_o_state_t *s);

int dlmm_o_side_matches(const dlmm_o_bin_t *bin, int swap_for_y);

void dlmm_o_available(const dlmm_o_bin_t *bin, int swap_for_y, int orders_on,
                      uint64_t *mm, uint64_t *proc, uint64_t *open_amt);

/* Layer fills. fill_mm is the CORE-003 helper; do not merge order debit into it. */
int dlmm_o_fill_mm(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
                   uint64_t *used_in, uint64_t *left, uint64_t *out_amt);
int dlmm_o_fill_processed_order(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
                                int orders_on, uint64_t *used_in, uint64_t *left,
                                uint64_t *out_amt);
int dlmm_o_fill_open_orders(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
                            int orders_on, uint64_t *used_in, uint64_t *left,
                            uint64_t *out_amt);

int dlmm_o_fill_bin(const dlmm_o_bin_t *bin, uint64_t amount, int swap_for_y,
                    int orders_on, dlmm_o_fill_t *out);

/* Debit each layer separately. Does not add order takes to amount_x/y. */
int dlmm_o_apply_fill(dlmm_o_bin_t *bin, dlmm_o_state_t *s, int swap_for_y,
                      const dlmm_o_fill_t *f);

int dlmm_o_apply_swap(const dlmm_o_state_t *before,
                      const dlmm_swap_ix_t *ix,
                      dlmm_o_state_t *after,
                      dlmm_apply_result_t *res);

int dlmm_o_quote_exact_in(const dlmm_o_state_t *state,
                          uint64_t amount_in,
                          uint8_t swap_for_y,
                          dlmm_quote_t *out);

#endif /* ARB_CORE_DLMM_ORDERS_H */
