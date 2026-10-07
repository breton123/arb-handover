#include "tx_template.h"

#include <string.h>

/*
 * EXEC-001 writable / signer flags. Compile uses these once.
 * Hot path never reads them.
 */
static const uint8_t ACC_SIGNER[ROUTE0_N] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
};

static const uint8_t ACC_WRITE[ROUTE0_N] = {
    1, /* authority */
    1, /* user_quote */
    1, /* user_base */
    0, 0, 0, 0, 0, 0,
    1, /* dlmm_lb_pair */
    0,
    1, /* reserve_x */
    1, /* reserve_y */
    1, /* oracle */
    0, 0, 0,
    1, /* bin_0 */
    1, /* bin_1 */
    0, 0,
    1, /* pump_pool */
    0, 0, 0, 0, 0,
    1, /* pool_base */
    1, /* pool_quote */
    0,
    1, /* proto_fee_ata */
    1, /* creator_ata */
    0, 0,
    1  /* user_vol */
};

static const uint8_t COMPUTE_BUDGET[32] = ROUTE0_COMPUTE_BUDGET_ID;

static int
put_u8(uint8_t *buf, uint16_t *cur, uint16_t cap, uint8_t v)
{
    if ((uint32_t)*cur + 1u > cap) {
        return -1;
    }
    buf[(*cur)++] = v;
    return 0;
}

static int
put_bytes(uint8_t *buf, uint16_t *cur, uint16_t cap, const uint8_t *p, uint16_t n)
{
    if ((uint32_t)*cur + n > cap) {
        return -1;
    }
    memcpy(buf + *cur, p, n);
    *cur = (uint16_t)(*cur + n);
    return 0;
}

static int
put_cu16(uint8_t *buf, uint16_t *cur, uint16_t cap, uint16_t v)
{
    if (v >= 128u) {
        return -1;
    }
    return put_u8(buf, cur, cap, (uint8_t)v);
}

static int
find_key(const uint8_t *keys, uint8_t n, const uint8_t *pk)
{
    uint8_t i;

    for (i = 0; i < n; i++) {
        if (memcmp(keys + (size_t)i * 32u, pk, 32) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int
add_key(uint8_t keys[][32], uint8_t *n, const uint8_t *pk, uint8_t *idx)
{
    int found;

    found = find_key(&keys[0][0], *n, pk);
    if (found >= 0) {
        *idx = (uint8_t)found;
        return 0;
    }
    if (*n >= ROUTE0_TX_NKEYS) {
        return -1;
    }
    memcpy(keys[*n], pk, 32);
    *idx = *n;
    (*n)++;
    return 0;
}

static int
want_pass(uint8_t signer, uint8_t writable, int pass)
{
    switch (pass) {
    case 0:
        return signer && writable;
    case 1:
        return signer && !writable;
    case 2:
        return !signer && writable;
    default:
        return !signer && !writable;
    }
}

#define KEY(i)  (keys + (size_t)(i) * 32u)

int
route0_tx_compile(const uint8_t *keys,
                  const uint8_t *our_exec,
                  uint32_t cu_limit,
                  route0_tx_template_t *out)
{
    uint8_t uniq[ROUTE0_TX_NKEYS][32];
    uint8_t map[ROUTE0_N];
    uint8_t ix_accs[ROUTE0_N];
    uint8_t nuniq = 0;
    uint8_t n_writable = 0;
    uint8_t exec_idx = 0;
    uint8_t cu_idx = 0;
    uint8_t ixdata[ROUTE0_IX_LEN];
    uint8_t n_ro_unsigned = 0;
    uint8_t zerosig[64];
    uint8_t z32[32];
    uint8_t pass;
    uint32_t i;
    uint16_t cur;
    uint16_t off_bh, off_price, off_dir, off_amt, off_min;
    uint16_t off_nacc, off_accs, off_data;
    opportunity_t dummy;
    size_t ixlen = 0;

    if (keys == NULL || our_exec == NULL || out == NULL) {
        return -1;
    }
    if (memcmp(KEY(ROUTE0_ACC_PUMP_BASE_MINT),
               KEY(ROUTE0_ACC_DLMM_TOKEN_X_MINT), 32) != 0) {
        return -1;
    }
    if (memcmp(KEY(ROUTE0_ACC_PUMP_QUOTE_MINT),
               KEY(ROUTE0_ACC_DLMM_TOKEN_Y_MINT), 32) != 0) {
        return -1;
    }

    memset(uniq, 0, sizeof(uniq));
    memset(out, 0, sizeof(*out));

    for (pass = 0; pass < 4; pass++) {
        for (i = 0; i < ROUTE0_N; i++) {
            if (!want_pass(ACC_SIGNER[i], ACC_WRITE[i], (int)pass)) {
                continue;
            }
            if (add_key(uniq, &nuniq, KEY(i), &map[i]) != 0) {
                return -1;
            }
        }
        if (pass == 2) {
            n_writable = nuniq;
        }
        if (pass == 3) {
            if (add_key(uniq, &nuniq, our_exec, &exec_idx) != 0) {
                return -1;
            }
            if (add_key(uniq, &nuniq, COMPUTE_BUDGET, &cu_idx) != 0) {
                return -1;
            }
        }
    }
    if (nuniq != ROUTE0_TX_NKEYS) {
        return -1;
    }
    if (map[ROUTE0_ACC_PUMP_BASE_MINT] != map[ROUTE0_ACC_DLMM_TOKEN_X_MINT] ||
        map[ROUTE0_ACC_PUMP_QUOTE_MINT] != map[ROUTE0_ACC_DLMM_TOKEN_Y_MINT]) {
        return -1;
    }
    if (n_writable != 15u) {
        return -1;
    }
    n_ro_unsigned = (uint8_t)(nuniq - n_writable);
    if (n_ro_unsigned != ROUTE0_TX_N_RO_UNSIGNED) {
        return -1;
    }

    memset(&dummy, 0, sizeof(dummy));
    dummy.route_id = ROUTE_DLMM_PUMP;
    dummy.valid    = 1;
    if (route0_pack(&dummy, 0, ixdata, sizeof(ixdata), &ixlen) != 0 ||
        ixlen != ROUTE0_IX_LEN) {
        return -1;
    }
    for (i = 0; i < ROUTE0_N; i++) {
        ix_accs[i] = map[i];
    }

    memset(zerosig, 0, sizeof(zerosig));
    memset(z32, 0, sizeof(z32));
    cur = 0;

    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, 1) != 0) {
        return -1;
    }
    if (put_bytes(out->bytes, &cur, ROUTE0_TX_LEN, zerosig, 64) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, 1) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, 0) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, ROUTE0_TX_N_RO_UNSIGNED) != 0) {
        return -1;
    }
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, ROUTE0_TX_NKEYS) != 0) {
        return -1;
    }
    if (cur != ROUTE0_OFF_KEYS) {
        return -1;
    }
    for (i = 0; i < ROUTE0_TX_NKEYS; i++) {
        if (put_bytes(out->bytes, &cur, ROUTE0_TX_LEN, uniq[i], 32) != 0) {
            return -1;
        }
    }

    off_bh = cur;
    if (put_bytes(out->bytes, &cur, ROUTE0_TX_LEN, z32, 32) != 0) {
        return -1;
    }
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, 3) != 0) {
        return -1;
    }

    /* ix0: SetComputeUnitLimit */
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, cu_idx) != 0) {
        return -1;
    }
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, 0) != 0) {
        return -1;
    }
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, 5) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, 2) != 0) {
        return -1;
    }
    if (put_bytes(out->bytes, &cur, ROUTE0_TX_LEN,
                  (const uint8_t *)&cu_limit, 4) != 0) {
        return -1;
    }

    /* ix1: SetComputeUnitPrice — price starts at cur+1 after disc */
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, cu_idx) != 0) {
        return -1;
    }
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, 0) != 0) {
        return -1;
    }
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, 9) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, 3) != 0) {
        return -1;
    }
    off_price = cur;
    if (put_bytes(out->bytes, &cur, ROUTE0_TX_LEN, z32, 8) != 0) {
        return -1;
    }

    /* ix2: OUR_EXEC route0 */
    if (put_u8(out->bytes, &cur, ROUTE0_TX_LEN, exec_idx) != 0) {
        return -1;
    }
    off_nacc = cur;
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, ROUTE0_N) != 0) {
        return -1;
    }
    off_accs = cur;
    if (put_bytes(out->bytes, &cur, ROUTE0_TX_LEN, ix_accs, ROUTE0_N) != 0) {
        return -1;
    }
    if (put_cu16(out->bytes, &cur, ROUTE0_TX_LEN, ROUTE0_IX_LEN) != 0) {
        return -1;
    }
    off_data = cur;
    if (put_bytes(out->bytes, &cur, ROUTE0_TX_LEN, ixdata, ROUTE0_IX_LEN) != 0) {
        return -1;
    }
    off_dir = (uint16_t)(off_data + 8u);
    off_amt = (uint16_t)(off_data + 9u);
    off_min = (uint16_t)(off_data + 17u);

    if (cur != ROUTE0_TX_LEN) {
        return -1;
    }
    if (off_bh != ROUTE0_OFF_BLOCKHASH ||
        off_price != ROUTE0_OFF_CU_PRICE ||
        off_dir != ROUTE0_OFF_DIRECTION ||
        off_amt != ROUTE0_OFF_AMOUNT_IN ||
        off_min != ROUTE0_OFF_MIN_PROFIT ||
        off_nacc != ROUTE0_OFF_ROUTE0_NACC ||
        off_accs != ROUTE0_OFF_ROUTE0_ACCS ||
        off_data != ROUTE0_OFF_ROUTE0_DATA) {
        return -1;
    }
    (void)exec_idx;
    return 0;
}
