#ifndef ARB_EXEC_SMOKE_TX_H
#define ARB_EXEC_SMOKE_TX_H

#include "sign.h"

#include <stdint.h>

#define SMOKE_TX_MAX  512u
#define SMOKE_MEMO    "arb-exec-005a"

typedef struct {
    uint8_t  bytes[SMOKE_TX_MAX];
    uint16_t len;
    uint16_t msg_off;
    uint16_t msg_len;
    uint16_t nsig;
} smoke_tx_t;

int smoke_build_memo(const uint8_t payer[32], const uint8_t blockhash[32],
                     uint64_t cu_price, smoke_tx_t *out);

int smoke_build_create_nonce(const uint8_t payer[32],
                             const uint8_t nonce_pk[32],
                             const uint8_t blockhash[32],
                             uint64_t rent, uint64_t cu_price,
                             smoke_tx_t *out);

int smoke_build_nonce_memo(const uint8_t payer[32],
                           const uint8_t nonce_acc[32],
                           const uint8_t nonce_hash[32],
                           uint64_t cu_price, smoke_tx_t *out);

int smoke_sign(const route0_signer_t *signers, uint8_t nsig, smoke_tx_t *tx);

#endif /* ARB_EXEC_SMOKE_TX_H */
