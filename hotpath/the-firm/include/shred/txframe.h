#ifndef FIRM_SHRED_TXFRAME_H
#define FIRM_SHRED_TXFRAME_H

#include <stdint.h>

/*
 * Wire-frame one Solana transaction. No ALT resolve, no ix decode,
 * no program recognition. Proven layout: TriggerResearch
 * analyze_shred_state.transaction / arb-core frame_v1_layout.
 *
 * v1 starts with 0x81 and carries signatures after the message.
 * That byte is not a compact-u16 continuation.
 */

#define TXF_OK                     0
#define TXF_NEED_MORE_BYTES        1
#define TXF_BAD_SHORTVEC           2
#define TXF_BAD_SIGNATURE_SECTION  3
#define TXF_UNSUPPORTED_VERSION    4
#define TXF_BAD_MESSAGE            5

#define TXF_ENC_LEGACY 0u
#define TXF_ENC_V0     1u
#define TXF_ENC_V1     2u

#define TXF_V1_PREFIX  0x81u
#define TXF_V1_HDR     42u
#define TXF_V1_MAX     4096u
#define TXF_V0_MAX     1232u
#define TXF_KEY_MAX    256u
#define TXF_SIG_MAX    16u
#define TXF_IX_MAX     256u

typedef struct {
    uint32_t tx_len;
    uint32_t nsig;
    uint32_t nkeys;
    uint32_t nix;
    uint32_t message_off;
    uint32_t stream_off;
    uint8_t  version;
    uint8_t  sig[64];
    uint8_t  msg_prefix[8];
} txframe_t;

const char *txframe_name(int rc);

/* Frame one tx at bytes[0..len). Does not mutate later-stage state. */
int txframe_parse(const uint8_t *bytes, uint32_t len, txframe_t *out);

#endif
