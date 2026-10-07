#include "route0_v0.h"

#include <string.h>

/*
 * Locked OUR_EXEC index vector. Logical 0..34 = EXEC-001 order.
 * 10,14 optional-none → static DLMM. 25,26 mint aliases.
 * 35 Token-2022. 36..57 Pump IDL.
 */
static const uint8_t IX_IDX[V0_N_IXACC] = {
    0,  11, 12, 3,  4,  5,  6,  7,  25, 13,
    7,  14, 15, 16, 7,  26, 27, 17, 18, 8,
    28, 19, 29, 30, 10, 26, 27, 20, 21, 31,
    22, 23, 32, 33, 24,
    9,
    19, 0,  29, 26, 27, 12, 11, 20, 21, 31,
    22, 9,  3,  4,  5,  28, 8,  23, 32, 33,
    10, 30
};

static const uint8_t DISC[8] = ROUTE0_DISC;

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

int
route0_v0_compile(const uint8_t *static_keys,
                  const uint8_t *alt,
                  const uint8_t *alt_wr,
                  const uint8_t *alt_ro,
                  uint32_t cu_limit,
                  route0_v0_template_t *out)
{
    uint8_t ixdata[ROUTE0_IX_LEN];
    uint8_t zerosig[64];
    uint8_t z32[32];
    uint16_t cur;
    uint16_t off_bh, off_price, off_dir, off_amt, off_min;
    uint32_t i;

    if (static_keys == NULL || alt == NULL || alt_wr == NULL ||
        alt_ro == NULL || out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    memset(zerosig, 0, sizeof(zerosig));
    memset(z32, 0, sizeof(z32));
    memcpy(ixdata, DISC, 8);
    memset(ixdata + 8, 0, ROUTE0_IX_LEN - 8);

    cur = 0;
    if (put_u8(out->bytes, &cur, V0_TX_LEN, 1) != 0) {
        return -1;
    }
    if (put_bytes(out->bytes, &cur, V0_TX_LEN, zerosig, 64) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, 0x80) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, 1) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, 0) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, (uint8_t)(V0_N_STATIC - 1u)) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, (uint8_t)V0_N_STATIC) != 0) {
        return -1;
    }
    for (i = 0; i < V0_N_STATIC; i++) {
        if (put_bytes(out->bytes, &cur, V0_TX_LEN, static_keys + i * 32u, 32) != 0) {
            return -1;
        }
    }
    off_bh = cur;
    if (put_bytes(out->bytes, &cur, V0_TX_LEN, z32, 32) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, 3) != 0) {
        return -1;
    }

    if (put_u8(out->bytes, &cur, V0_TX_LEN, V0_STATIC_CU) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, 0) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, 5) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, 2) != 0 ||
        put_bytes(out->bytes, &cur, V0_TX_LEN, (const uint8_t *)&cu_limit, 4) != 0) {
        return -1;
    }

    if (put_u8(out->bytes, &cur, V0_TX_LEN, V0_STATIC_CU) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, 0) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, 9) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, 3) != 0) {
        return -1;
    }
    off_price = cur;
    if (put_bytes(out->bytes, &cur, V0_TX_LEN, z32, 8) != 0) {
        return -1;
    }

    if (put_u8(out->bytes, &cur, V0_TX_LEN, V0_STATIC_OUR_EXEC) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, (uint8_t)V0_N_IXACC) != 0 ||
        put_bytes(out->bytes, &cur, V0_TX_LEN, IX_IDX, V0_N_IXACC) != 0 ||
        put_u8(out->bytes, &cur, V0_TX_LEN, ROUTE0_IX_LEN) != 0) {
        return -1;
    }
    if (cur != V0_OFF_DIRECTION - 8u) {
        return -1;
    }
    off_dir = (uint16_t)(cur + 8u);
    off_amt = (uint16_t)(cur + 9u);
    off_min = (uint16_t)(cur + 17u);
    if (put_bytes(out->bytes, &cur, V0_TX_LEN, ixdata, ROUTE0_IX_LEN) != 0) {
        return -1;
    }

    if (put_u8(out->bytes, &cur, V0_TX_LEN, 1) != 0) {
        return -1;
    }
    if (put_bytes(out->bytes, &cur, V0_TX_LEN, alt, 32) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, (uint8_t)V0_N_ALT_WR) != 0) {
        return -1;
    }
    for (i = 0; i < V0_N_ALT_WR; i++) {
        if (put_u8(out->bytes, &cur, V0_TX_LEN, (uint8_t)i) != 0) {
            return -1;
        }
    }
    if (put_u8(out->bytes, &cur, V0_TX_LEN, (uint8_t)V0_N_ALT_RO) != 0) {
        return -1;
    }
    for (i = 0; i < V0_N_ALT_RO; i++) {
        if (put_u8(out->bytes, &cur, V0_TX_LEN, (uint8_t)(V0_N_ALT_WR + i)) != 0) {
            return -1;
        }
    }

    if (cur != V0_TX_LEN) {
        return -1;
    }
    if (off_bh != V0_OFF_BLOCKHASH || off_price != V0_OFF_CU_PRICE ||
        off_dir != V0_OFF_DIRECTION || off_amt != V0_OFF_AMOUNT_IN ||
        off_min != V0_OFF_MIN_PROFIT) {
        return -1;
    }
    (void)alt_wr;
    (void)alt_ro;
    return 0;
}
