#include "route0.h"

#include <string.h>

const char *const route0_acc_name[ROUTE0_N] = {
    "authority",
    "user_quote",
    "user_base",
    "token_program",
    "system_program",
    "ata_program",
    "memo_program",
    "dlmm_program",
    "dlmm_event_auth",
    "dlmm_lb_pair",
    "dlmm_bitmap",
    "dlmm_reserve_x",
    "dlmm_reserve_y",
    "dlmm_oracle",
    "dlmm_host_fee",
    "dlmm_token_x_mint",
    "dlmm_token_y_mint",
    "dlmm_bin_0",
    "dlmm_bin_1",
    "pump_program",
    "pump_event_auth",
    "pump_pool",
    "pump_global",
    "pump_fee_config",
    "pump_fee_program",
    "pump_base_mint",
    "pump_quote_mint",
    "pump_pool_base",
    "pump_pool_quote",
    "pump_proto_fee",
    "pump_proto_fee_ata",
    "pump_creator_ata",
    "pump_creator_auth",
    "pump_global_vol",
    "pump_user_vol",
};

static const uint8_t DISC[8] = ROUTE0_DISC;

static uint64_t
load_u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void
store_u64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

int
route0_pack(const opportunity_t *opp, uint64_t min_profit,
            uint8_t *buf, size_t cap, size_t *out_len)
{
    if (opp == NULL || buf == NULL || out_len == NULL) {
        return ROUTE0_ERR_IX;
    }
    if (opp->route_id != ROUTE_DLMM_PUMP || !opp->valid) {
        return ROUTE0_ERR_IX;
    }
    if (opp->direction > 1u) {
        return ROUTE0_ERR_IX;
    }
    if (cap < ROUTE0_IX_LEN) {
        return ROUTE0_ERR_IX;
    }
    memcpy(buf, DISC, 8);
    buf[8] = opp->direction;
    store_u64(buf + 9, opp->amount_in);
    store_u64(buf + 17, min_profit);
    *out_len = ROUTE0_IX_LEN;
    return ROUTE0_OK;
}

static int
tok_amt(const exec_account_t *a, uint64_t *out)
{
    if (a == NULL || a->dlen < SPL_AMOUNT_OFF + 8) {
        return -1;
    }
    *out = load_u64(a->data + SPL_AMOUNT_OFF);
    return 0;
}

static int
tok_set(exec_account_t *a, uint64_t v)
{
    if (a == NULL || !a->writable || a->dlen < SPL_AMOUNT_OFF + 8) {
        return -1;
    }
    store_u64(a->data + SPL_AMOUNT_OFF, v);
    return 0;
}

static int
transfer(exec_account_t *from, exec_account_t *to, uint64_t n, uint64_t *cu)
{
    uint64_t a, b;

    if (from == NULL || to == NULL) {
        return -1;
    }
    if (memcmp(from->data + SPL_MINT_OFF, to->data + SPL_MINT_OFF, 32) != 0) {
        return -1;
    }
    if (tok_amt(from, &a) != 0 || tok_amt(to, &b) != 0) {
        return -1;
    }
    if (a < n || b > UINT64_MAX - n) {
        return -1;
    }
    if (tok_set(from, a - n) != 0 || tok_set(to, b + n) != 0) {
        return -1;
    }
    *cu += ROUTE0_CU_TRANSFER;
    return 0;
}

static int
mul_div(uint64_t amt, uint64_t num, uint64_t den, uint64_t *out)
{
    if (den == 0u) {
        return -1;
    }
    if (num != 0u && amt > UINT64_MAX / num) {
        return -1;
    }
    *out = (amt * num) / den;
    return 0;
}

static int
rate_out(const exec_account_t *cfg, uint64_t amount_in, uint64_t *out)
{
    uint64_t num, den;

    if (cfg == NULL || cfg->dlen < 17) {
        return -1;
    }
    if (cfg->data[16] != 0) {
        return -1; /* fail flag — stale / disabled venue */
    }
    num = load_u64(cfg->data);
    den = load_u64(cfg->data + 8);
    return mul_div(amount_in, num, den, out);
}

/*
 * DLMM swap2 slice: pair, bitmap, reserves, user in/out, mints, oracle,
 * host fee, user, token program, memo, event auth, program, two bin arrays.
 * Shims move tokens only. Live program replaces this function, not the metas.
 */
static int
cpi_dlmm(exec_account_t *acc, uint64_t amount_in, uint8_t swap_for_y, uint64_t *cu)
{
    exec_account_t *user_in, *user_out, *res_in, *res_out;
    uint64_t out;

    *cu += ROUTE0_CU_CPI;
    if (rate_out(&acc[ROUTE0_ACC_DLMM_LB_PAIR], amount_in, &out) != 0) {
        return ROUTE0_ERR_DLMM;
    }
    if (swap_for_y) {
        user_in  = &acc[ROUTE0_ACC_USER_BASE];
        user_out = &acc[ROUTE0_ACC_USER_QUOTE];
        res_in   = &acc[ROUTE0_ACC_DLMM_RESERVE_X];
        res_out  = &acc[ROUTE0_ACC_DLMM_RESERVE_Y];
    } else {
        user_in  = &acc[ROUTE0_ACC_USER_QUOTE];
        user_out = &acc[ROUTE0_ACC_USER_BASE];
        res_in   = &acc[ROUTE0_ACC_DLMM_RESERVE_Y];
        res_out  = &acc[ROUTE0_ACC_DLMM_RESERVE_X];
    }
    if (transfer(user_in, res_in, amount_in, cu) != 0) {
        return ROUTE0_ERR_TOKEN;
    }
    if (transfer(res_out, user_out, out, cu) != 0) {
        return ROUTE0_ERR_TOKEN;
    }
    (void)acc[ROUTE0_ACC_DLMM_PROGRAM];
    (void)acc[ROUTE0_ACC_DLMM_EVENT_AUTH];
    (void)acc[ROUTE0_ACC_DLMM_BITMAP];
    (void)acc[ROUTE0_ACC_DLMM_ORACLE];
    (void)acc[ROUTE0_ACC_DLMM_HOST_FEE];
    (void)acc[ROUTE0_ACC_DLMM_TOKEN_X_MINT];
    (void)acc[ROUTE0_ACC_DLMM_TOKEN_Y_MINT];
    (void)acc[ROUTE0_ACC_DLMM_BIN_0];
    (void)acc[ROUTE0_ACC_DLMM_BIN_1];
    (void)acc[ROUTE0_ACC_MEMO_PROGRAM];
    return ROUTE0_OK;
}

/*
 * Pump buy (dir 1) / sell (dir 0) slice. Same metas as the live IDL.
 */
static int
cpi_pump(exec_account_t *acc, uint64_t amount_in, uint8_t sell, uint64_t *cu)
{
    exec_account_t *user_in, *user_out, *pool_in, *pool_out;
    uint64_t out;

    *cu += ROUTE0_CU_CPI;
    if (rate_out(&acc[ROUTE0_ACC_PUMP_POOL], amount_in, &out) != 0) {
        return ROUTE0_ERR_PUMP;
    }
    if (sell) {
        user_in  = &acc[ROUTE0_ACC_USER_BASE];
        user_out = &acc[ROUTE0_ACC_USER_QUOTE];
        pool_in  = &acc[ROUTE0_ACC_PUMP_POOL_BASE];
        pool_out = &acc[ROUTE0_ACC_PUMP_POOL_QUOTE];
    } else {
        user_in  = &acc[ROUTE0_ACC_USER_QUOTE];
        user_out = &acc[ROUTE0_ACC_USER_BASE];
        pool_in  = &acc[ROUTE0_ACC_PUMP_POOL_QUOTE];
        pool_out = &acc[ROUTE0_ACC_PUMP_POOL_BASE];
    }
    if (transfer(user_in, pool_in, amount_in, cu) != 0) {
        return ROUTE0_ERR_TOKEN;
    }
    if (transfer(pool_out, user_out, out, cu) != 0) {
        return ROUTE0_ERR_TOKEN;
    }
    (void)acc[ROUTE0_ACC_PUMP_PROGRAM];
    (void)acc[ROUTE0_ACC_PUMP_EVENT_AUTH];
    (void)acc[ROUTE0_ACC_PUMP_GLOBAL];
    (void)acc[ROUTE0_ACC_PUMP_FEE_CONFIG];
    (void)acc[ROUTE0_ACC_PUMP_FEE_PROGRAM];
    (void)acc[ROUTE0_ACC_PUMP_BASE_MINT];
    (void)acc[ROUTE0_ACC_PUMP_QUOTE_MINT];
    (void)acc[ROUTE0_ACC_PUMP_PROTO_FEE];
    (void)acc[ROUTE0_ACC_PUMP_PROTO_FEE_ATA];
    (void)acc[ROUTE0_ACC_PUMP_CREATOR_ATA];
    (void)acc[ROUTE0_ACC_PUMP_CREATOR_AUTH];
    (void)acc[ROUTE0_ACC_PUMP_GLOBAL_VOL];
    (void)acc[ROUTE0_ACC_PUMP_USER_VOL];
    (void)acc[ROUTE0_ACC_SYSTEM_PROGRAM];
    (void)acc[ROUTE0_ACC_ATA_PROGRAM];
    (void)acc[ROUTE0_ACC_TOKEN_PROGRAM];
    return ROUTE0_OK;
}

int
route0_process(exec_account_t *acc, uint32_t nacc,
               const uint8_t *ix, size_t ixlen,
               route0_result_t *out)
{
    exec_account_t snap[ROUTE0_N];
    uint64_t amount_in, min_profit, q0, q1, b0, b1, cu;
    uint8_t direction;
    int rc;

    if (out == NULL) {
        return ROUTE0_ERR_IX;
    }
    memset(out, 0, sizeof(*out));
    if (acc == NULL || ix == NULL || ixlen != ROUTE0_IX_LEN) {
        out->rc = ROUTE0_ERR_IX;
        return ROUTE0_ERR_IX;
    }
    if (memcmp(ix, DISC, 8) != 0) {
        out->rc = ROUTE0_ERR_IX;
        return ROUTE0_ERR_IX;
    }
    if (nacc != ROUTE0_N) {
        out->rc = ROUTE0_ERR_ACCOUNTS;
        return ROUTE0_ERR_ACCOUNTS;
    }
    if (!acc[ROUTE0_ACC_AUTHORITY].signer) {
        out->rc = ROUTE0_ERR_SIGNER;
        return ROUTE0_ERR_SIGNER;
    }

    direction  = ix[8];
    amount_in  = load_u64(ix + 9);
    min_profit = load_u64(ix + 17);
    if (direction > 1u || amount_in == 0u) {
        out->rc = ROUTE0_ERR_IX;
        return ROUTE0_ERR_IX;
    }

    if (tok_amt(&acc[ROUTE0_ACC_USER_QUOTE], &q0) != 0 ||
        tok_amt(&acc[ROUTE0_ACC_USER_BASE], &b0) != 0) {
        out->rc = ROUTE0_ERR_TOKEN;
        return ROUTE0_ERR_TOKEN;
    }
    out->quote_before = q0;
    out->base_before  = b0;
    memcpy(snap, acc, sizeof(snap));

    cu = ROUTE0_CU_ENTRY + (uint64_t)ROUTE0_N * ROUTE0_CU_ACCOUNT;
    if (direction == ROUTE0_DIR_DLMM_THEN_PUMP) {
        rc = cpi_dlmm(acc, amount_in, 0, &cu);
        if (rc == ROUTE0_OK) {
            uint64_t mid;
            if (tok_amt(&acc[ROUTE0_ACC_USER_BASE], &mid) != 0) {
                rc = ROUTE0_ERR_TOKEN;
            } else {
                rc = cpi_pump(acc, mid, 1, &cu);
            }
        }
    } else {
        rc = cpi_pump(acc, amount_in, 0, &cu);
        if (rc == ROUTE0_OK) {
            uint64_t mid;
            if (tok_amt(&acc[ROUTE0_ACC_USER_BASE], &mid) != 0) {
                rc = ROUTE0_ERR_TOKEN;
            } else {
                rc = cpi_dlmm(acc, mid, 1, &cu);
            }
        }
    }

    if (rc != ROUTE0_OK) {
        memcpy(acc, snap, sizeof(snap));
        out->rc = rc;
        out->cu = cu;
        out->quote_after = q0;
        out->base_after  = b0;
        return rc;
    }

    cu += ROUTE0_CU_GUARD;
    if (tok_amt(&acc[ROUTE0_ACC_USER_QUOTE], &q1) != 0 ||
        tok_amt(&acc[ROUTE0_ACC_USER_BASE], &b1) != 0) {
        memcpy(acc, snap, sizeof(snap));
        out->rc = ROUTE0_ERR_TOKEN;
        out->cu = cu;
        out->quote_after = q0;
        out->base_after  = b0;
        return ROUTE0_ERR_TOKEN;
    }
    if (q1 < q0 || (q1 - q0) < min_profit) {
        memcpy(acc, snap, sizeof(snap));
        out->rc = ROUTE0_ERR_PROFIT;
        out->cu = cu;
        out->quote_after = q0;
        out->base_after  = b0;
        return ROUTE0_ERR_PROFIT;
    }

    out->rc          = ROUTE0_OK;
    out->cu          = cu;
    out->quote_after = q1;
    out->base_after  = b1;
    return ROUTE0_OK;
}
