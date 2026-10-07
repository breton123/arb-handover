#ifndef ARB_CORE_FRAME_H
#define ARB_CORE_FRAME_H

#include "tx.h"

#include <stdint.h>

/*
 * CORE-010 — early transaction framing beside locked swapix / tx.c.
 *
 * CORE-001 finds interesting bytes. This asks whether those bytes
 * already constitute a complete serialized Solana transaction in the
 * currently available shred payload. It does not wait for Entry /
 * FEC / slot completion and does not call copy_sig.
 *
 *   FRAME_INVALID     structure contradicts — permanent reject
 *   FRAME_INCOMPLETE  truncated — wait for another fragment
 *   FRAME_FRAMED      full tx, signatures[0] == candidate, dex ix inside
 */

#define FRAME_INVALID     0
#define FRAME_INCOMPLETE  1
#define FRAME_FRAMED      2

#define FRAME_VER_LEGACY  0u
#define FRAME_VER_V0      1u
#define FRAME_VER_V1      2u

#define TX_V1_PREFIX      0x81u
#define TX_V1_MAX         4096u
#define TX_V1_CFG_KNOWN   0x1fu
#define TX_V1_HDR_SIZE    42u

typedef struct {
    uint8_t  klass;
    uint8_t  nsig;
    uint8_t  versioned; /* FRAME_VER_* — not a boolean */
    uint8_t  have_dex;
    uint16_t nkeys;
    uint16_t ninstr;
    uint32_t tx_off;
    uint32_t tx_end;
} frame_hit_t;

/*
 * SIMD-0385 v1 layout offsets in `p` (absolute).
 * Signatures are at the tail. No address lookup tables.
 */
typedef struct {
    uint8_t  nsig;
    uint8_t  ninstr;
    uint8_t  naddr;
    uint32_t mask;
    uint32_t keys_off;
    uint32_t cfg_off;
    uint32_t hdr_off;
    uint32_t pay_off;
    uint32_t sig_off;
    uint32_t tx_end;
} frame_v1_t;

/* Parse one tx starting at `start`. 0 framed, 1 incomplete, -1 invalid.
 * Fast path: requires a DLMM/Pump program id in the STATIC key table. */
int frame_try_at(const uint8_t *p, uint32_t len, uint32_t start,
                 frame_hit_t *out);

/*
 * Generic structural frame. Same CORE-010 rules, but:
 *   - does not require a static DEX program id
 *   - v0 instruction indexes may address ALT-loaded keys
 * FRAMED here means "complete serialized tx", not "we already have N".
 */
int frame_try_any(const uint8_t *p, uint32_t len, uint32_t start,
                  frame_hit_t *out);

int frame_around_sig_any(const uint8_t *p, uint32_t len, const uint8_t sig[64],
                         frame_hit_t *out);

/*
 * Candidate 64-byte slice must be signatures[0] of a locally framed tx.
 * Searches `p` for `sig` and tries shortvec starts immediately before it.
 */
int frame_around_sig(const uint8_t *p, uint32_t len, const uint8_t sig[64],
                     frame_hit_t *out);

/* Structural v1 parse. 0 framed, 1 incomplete, -1 invalid. */
int frame_v1_layout(const uint8_t *p, uint32_t len, uint32_t start,
                    frame_v1_t *out);

/*
 * Instruction i of a framed v1. Writes program index, account count,
 * data length, and absolute offsets of the account-index and data slices.
 */
int frame_v1_ix(const uint8_t *p, const frame_v1_t *L, uint8_t i,
                uint8_t *prog, uint8_t *nacct, uint16_t *dlen,
                uint32_t *acc_off, uint32_t *data_off);

#endif /* ARB_CORE_FRAME_H */
