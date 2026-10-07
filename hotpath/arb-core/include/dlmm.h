#ifndef ARB_CORE_DLMM_H
#define ARB_CORE_DLMM_H

#include <stdint.h>

/*
 * Meteora DLMM predicted-state kernel (CORE-003).
 * Protocol math from validation/reference/dlmm-sdk (commons quote + bin + lb_pair).
 *
 * Restricted: exact-in, classic SPL, no limit orders, no Token-2022 transfer fee.
 * Bins that the walk needs must already be in state. Missing bin → fail.
 */

#define DLMM_BINS_MAX          210
#define DLMM_FEE_PRECISION     1000000000ull
#define DLMM_MAX_FEE_RATE      100000000ull
#define DLMM_BASIS_POINT_MAX   10000u
#define DLMM_SCALE_OFFSET      64u
#define DLMM_MIN_BIN_ID        (-443636)
#define DLMM_MAX_BIN_ID        443636

#define DLMM_SWAP2_DISC        "\x41\x4b\x3f\x4c\xeb\x5b\x5b\x88"

typedef struct {
    uint16_t base_factor;
    uint16_t filter_period;
    uint16_t decay_period;
    uint16_t reduction_factor;
    uint32_t variable_fee_control;
    uint32_t max_volatility_accumulator;
    uint16_t protocol_share;
    uint8_t  base_fee_power_factor;
    uint8_t  collect_fee_mode; /* 0 InputOnly, 1 OnlyY */
} dlmm_static_params_t;

typedef struct {
    uint32_t volatility_accumulator;
    uint32_t volatility_reference;
    int32_t  index_reference;
    int64_t  last_update_timestamp;
} dlmm_vparams_t;

typedef struct {
    int32_t  id;
    uint64_t amount_x;
    uint64_t amount_y;
    unsigned __int128 price; /* 0 → compute from id + bin_step */
} dlmm_bin_t;

typedef struct {
    int32_t              active_id;
    uint16_t             bin_step;
    uint8_t              status; /* 0 enabled */
    dlmm_static_params_t parameters;
    dlmm_vparams_t       v_parameters;
    uint64_t             reserve_x;
    uint64_t             reserve_y;
    dlmm_bin_t           bins[DLMM_BINS_MAX];
    uint16_t             nbin;
    int64_t              now_ts;
} dlmm_state_t;

typedef struct {
    uint64_t amount_in;
    uint64_t min_amount_out;
    uint8_t  swap_for_y;
} dlmm_swap_ix_t;

typedef struct {
    uint64_t amount_in;
    uint64_t amount_out;
    uint64_t fee;
    uint64_t protocol_fee;
    int32_t  active_id_after;
} dlmm_apply_result_t;

/* CORE-005 — exact-in quote against a predicted state. Does not write *state. */
typedef struct {
    uint64_t amount_out;
    uint64_t fee;
    uint16_t bins_crossed;
    uint8_t  valid;
} dlmm_quote_t;

int dlmm_price_from_id(int32_t id, uint16_t bin_step, unsigned __int128 *out);

int dlmm_apply_swap(const dlmm_state_t *before,
                    const dlmm_swap_ix_t *ix,
                    dlmm_state_t *after,
                    dlmm_apply_result_t *res);

int dlmm_quote_exact_in(const dlmm_state_t *state,
                        uint64_t amount_in,
                        uint8_t swap_for_y,
                        dlmm_quote_t *out);

/* 8-byte disc + u64 amount_in + u64 min_out. Returns 0 if swap2. */
int dlmm_decode_swap2(const uint8_t *data, uint16_t len, dlmm_swap_ix_t *ix);

#endif /* ARB_CORE_DLMM_H */
