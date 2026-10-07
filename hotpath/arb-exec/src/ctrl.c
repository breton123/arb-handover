#include "ctrl.h"

#include <string.h>

void
ctrl_fees_clear(ctrl_fees_t *f)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
}

int
ctrl_fees_set(ctrl_fees_t *f, uint64_t cu_price, uint64_t min_profit)
{
    if (f == NULL) {
        return -1;
    }
    f->cu_price   = cu_price;
    f->min_profit = min_profit;
    return 0;
}

int
ctrl_nonce_load_all(nonce_pool_t *p, const ctrl_nonce_rec_t *rec, uint32_t n)
{
    uint32_t i;

    if (p == NULL || rec == NULL || n != NONCE_POOL_N) {
        return -1;
    }
    nonce_pool_init(p);
    for (i = 0; i < NONCE_POOL_N; i++) {
        if (nonce_load(p, i, rec[i].pubkey, rec[i].hash) != 0) {
            return -1;
        }
    }
    if (p->n_ready != NONCE_POOL_N) {
        return -1;
    }
    return 0;
}

int
ctrl_nonce_finish(nonce_pool_t *p, uint32_t idx, const uint8_t hash[32])
{
    return nonce_reload(p, idx, hash);
}

static uint32_t
load_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t
load_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint64_t
load_u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void
store_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void
store_u16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, 2);
}

static void
store_u64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

int
ctrl_bin_read(const uint8_t *buf, size_t len,
              ctrl_nonce_rec_t rec[NONCE_POOL_N], ctrl_fees_t *fees)
{
    uint32_t i;

    if (buf == NULL || rec == NULL || fees == NULL || len < CTRL_BIN_LEN) {
        return -1;
    }
    if (load_u32(buf) != CTRL_BIN_MAGIC || load_u16(buf + 4) != CTRL_BIN_VER) {
        return -1;
    }
    if (load_u16(buf + 6) != (uint16_t)NONCE_POOL_N) {
        return -1;
    }
    if (ctrl_fees_set(fees, load_u64(buf + 8), load_u64(buf + 16)) != 0) {
        return -1;
    }
    for (i = 0; i < NONCE_POOL_N; i++) {
        memcpy(rec[i].pubkey, buf + CTRL_BIN_HDR + (size_t)i * CTRL_BIN_REC, 32);
        memcpy(rec[i].hash, buf + CTRL_BIN_HDR + (size_t)i * CTRL_BIN_REC + 32, 32);
    }
    return 0;
}

int
ctrl_bin_write(uint8_t *buf, size_t cap,
               const ctrl_nonce_rec_t rec[NONCE_POOL_N],
               const ctrl_fees_t *fees, size_t *out_len)
{
    uint32_t i;

    if (buf == NULL || rec == NULL || fees == NULL || out_len == NULL) {
        return -1;
    }
    if (cap < CTRL_BIN_LEN) {
        return -1;
    }
    memset(buf, 0, CTRL_BIN_LEN);
    store_u32(buf, CTRL_BIN_MAGIC);
    store_u16(buf + 4, (uint16_t)CTRL_BIN_VER);
    store_u16(buf + 6, (uint16_t)NONCE_POOL_N);
    store_u64(buf + 8, fees->cu_price);
    store_u64(buf + 16, fees->min_profit);
    for (i = 0; i < NONCE_POOL_N; i++) {
        memcpy(buf + CTRL_BIN_HDR + (size_t)i * CTRL_BIN_REC, rec[i].pubkey, 32);
        memcpy(buf + CTRL_BIN_HDR + (size_t)i * CTRL_BIN_REC + 32, rec[i].hash, 32);
    }
    *out_len = CTRL_BIN_LEN;
    return 0;
}
