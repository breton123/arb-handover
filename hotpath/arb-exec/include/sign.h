#ifndef ARB_EXEC_SIGN_H
#define ARB_EXEC_SIGN_H

#include "tx_template.h"

#include <stdint.h>

/*
 * EXEC-003 — Ed25519 over the frozen message slice.
 *
 *   ed25519_sign(tx + ROUTE0_OFF_SIGNATURE,
 *                tx + ROUTE0_OFF_MESSAGE,
 *                ROUTE0_MSG_LEN,
 *                key);
 *
 * Signer state is prepared once. Hot path does not open files,
 * decode keys, derive the public key, or allocate.
 */

typedef struct {
    uint8_t sk[64];
    uint8_t pk[32];
} route0_signer_t;

int route0_signer_init(route0_signer_t *s, const uint8_t seed[32]);
int exec_sign(const route0_signer_t *s, uint8_t *sig,
              const uint8_t *msg, uint32_t msg_len);
int route0_sign(const route0_signer_t *s, uint8_t *tx);
int route0_verify_sodium(const uint8_t *tx, const uint8_t pk[32]);
int route0_verify_openssl(const uint8_t *tx, const uint8_t pk[32]);

typedef struct route0_ossl_signer route0_ossl_signer_t;

int route0_ossl_signer_init(route0_ossl_signer_t **out, const uint8_t seed[32]);
int route0_ossl_sign(route0_ossl_signer_t *s, uint8_t *tx);
void route0_ossl_signer_free(route0_ossl_signer_t *s);

#endif /* ARB_EXEC_SIGN_H */
