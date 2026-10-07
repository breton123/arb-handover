#ifndef FIRM_TX_VIEW_H
#define FIRM_TX_VIEW_H

#include <stdint.h>

/*
 * Message decode of a framed transaction. No Pump, no apply.
 * Instructions keep account indices and raw data aliases.
 */

#define TXV_OK           0
#define TXV_NEED_MORE    1
#define TXV_BAD          2
#define TXV_UNSUPPORTED  3

#define TXV_KEY_MAX  256u
#define TXV_IX_MAX   64u
#define TXV_ACC_MAX  64u
#define TXV_ALT_MAX  8u
#define TXV_IDX_MAX  64u

typedef struct {
    uint8_t        prog;
    uint8_t        nacc;
    uint8_t        acc[TXV_ACC_MAX];
    uint16_t       dlen;
    const uint8_t *data;
} tx_ix_t;

typedef struct {
    uint8_t table[32];
    uint8_t nw;
    uint8_t nr;
    uint8_t widx[TXV_IDX_MAX];
    uint8_t ridx[TXV_IDX_MAX];
} tx_alt_t;

typedef struct {
    uint8_t        sig[64];
    uint8_t        version;
    uint8_t        hdr_ok;
    uint8_t        nsig;
    uint8_t        nro_signed;
    uint8_t        nro_unsigned;
    uint16_t       n_static;
    uint16_t       n_keys;
    uint16_t       n_ix;
    uint8_t        n_alt;
    uint8_t        lut_missing;
    uint8_t        key[TXV_KEY_MAX][32];
    tx_ix_t        ix[TXV_IX_MAX];
    tx_alt_t       alt[TXV_ALT_MAX];
    const uint8_t *raw;
    uint32_t       raw_len;
} tx_view_t;

int txview_decode(const uint8_t *bytes, uint32_t len, tx_view_t *out);

#endif
