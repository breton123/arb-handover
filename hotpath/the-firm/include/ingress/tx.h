#ifndef FIRM_INGRESS_TX_H
#define FIRM_INGRESS_TX_H

#include <stdint.h>

/*
 * Normalized authenticated transaction. Reconstruction fills this;
 * the state engine does not parse shreds or talk to RPC.
 *
 * encoding names the Solana message form. v0 apply uses decoded
 * instructions; raw bytes are optional for later fallback/oracle.
 */

#define TX_ENC_LEGACY  0u
#define TX_ENC_V0      1u
#define TX_ENC_V1      2u

#define STATE_IX_MAX   32u
#define STATE_KEY_MAX  256u
#define STATE_ACC_MAX  16u
#define STATE_SIG_LEN  64u
#define STATE_ACCT_NONE UINT32_MAX

#define PROTO_NONE     0u
#define PROTO_PUMP     1u
#define PROTO_DLMM     2u
#define PROTO_TOKEN    3u
#define PROTO_ATA      4u
#define PROTO_SYSTEM   5u
#define PROTO_UNKNOWN  255u

#define IX_KIND_NONE           0u
#define IX_KIND_PUMP_SELL      1u /* exact-in base → quote */
#define IX_KIND_PUMP_BUY_EQ    2u /* buy_exact_quote_in */
#define IX_KIND_PUMP_BUY       3u /* exact-out buy: amount_in=base_out */
#define IX_KIND_DLMM_SWAP      4u /* TODO: FALLBACK */
#define IX_KIND_TOKEN_XFER     5u
#define IX_KIND_TOKEN_SYNC     6u /* SyncNative */
#define IX_KIND_TOKEN_CLOSE    7u
#define IX_KIND_ATA_CREATE     8u
#define IX_KIND_ATA_INIT       9u
#define IX_KIND_SYS_TRANSFER   10u
#define IX_KIND_OTHER          11u

typedef struct {
    uint8_t  proto;
    uint8_t  kind;
    uint8_t  prog;       /* index into tx->key */
    uint8_t  relevant;
    uint8_t  direction;  /* pump: 0 quote→base, 1 base→quote */
    uint32_t pool_id;
    uint32_t src_token;
    uint32_t dst_token;
    uint32_t vault_base;
    uint32_t vault_quote;
    uint32_t fee_proto;
    uint32_t fee_creator;
    uint32_t src_sys;
    uint32_t dst_sys;
    uint64_t amount_in;  /* exact-in quote/base, or exact-out base_out */
    uint64_t min_out;    /* min_out, or max_quote for exact-out buy */
    uint8_t  mint[32];
    uint8_t  owner[32];
} ordered_ix_t;

typedef struct {
    uint8_t      sig[STATE_SIG_LEN];
    uint64_t     slot;
    uint32_t     entry_index;
    uint32_t     tx_index;
    uint8_t      encoding;
    uint8_t      exec_failed;
    uint8_t      hdr_ok;
    uint8_t      nsig;
    uint8_t      nro_signed;
    uint8_t      nro_unsigned;
    uint16_t     n_ix;
    uint16_t     n_keys;
    uint16_t     n_static;
    uint16_t     n_lut_w;
    uint8_t      acc_n[STATE_IX_MAX];
    uint8_t      acc_ix[STATE_IX_MAX][STATE_ACC_MAX];
    uint8_t      key[STATE_KEY_MAX][32];
    ordered_ix_t ix[STATE_IX_MAX];
    const uint8_t *raw;
    uint32_t     raw_len;
} ordered_tx_t;

void ordered_tx_clear(ordered_tx_t *tx);

#endif /* FIRM_INGRESS_TX_H */
