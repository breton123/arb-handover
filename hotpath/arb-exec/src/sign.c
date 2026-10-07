#include "sign.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/*
 * libsodium 1.0.18 is on the box without headers. These are the
 * stable public ABI. We do not implement the math.
 */
#define SODIUM_SK_LEN  64u
#define SODIUM_PK_LEN  32u
#define SODIUM_SIG_LEN 64u

int sodium_init(void);
int crypto_sign_ed25519_seed_keypair(unsigned char *pk, unsigned char *sk,
                                     const unsigned char *seed);
int crypto_sign_ed25519_detached(unsigned char *sig,
                                 unsigned long long *siglen_p,
                                 const unsigned char *m,
                                 unsigned long long mlen,
                                 const unsigned char *sk);
int crypto_sign_ed25519_verify_detached(const unsigned char *sig,
                                        const unsigned char *m,
                                        unsigned long long mlen,
                                        const unsigned char *pk);

struct route0_ossl_signer {
    EVP_PKEY *pkey;
    EVP_MD_CTX *md;
};

int
route0_signer_init(route0_signer_t *s, const uint8_t seed[32])
{
    if (s == NULL || seed == NULL) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    if (sodium_init() < 0) {
        return -1;
    }
    if (crypto_sign_ed25519_seed_keypair(s->pk, s->sk, seed) != 0) {
        return -1;
    }
    (void)mlock(s, sizeof(*s));
    return 0;
}

int
exec_sign(const route0_signer_t *s, uint8_t *sig,
          const uint8_t *msg, uint32_t msg_len)
{
    unsigned long long slen = SODIUM_SIG_LEN;

    if (s == NULL || sig == NULL || msg == NULL || msg_len == 0) {
        return -1;
    }
    if (crypto_sign_ed25519_detached(sig, &slen, msg, msg_len, s->sk) != 0) {
        return -1;
    }
    if (slen != SODIUM_SIG_LEN) {
        return -1;
    }
    return 0;
}

int
route0_sign(const route0_signer_t *s, uint8_t *tx)
{
    if (s == NULL || tx == NULL) {
        return -1;
    }
    return exec_sign(s, tx + ROUTE0_OFF_SIGNATURE,
                     tx + ROUTE0_OFF_MESSAGE, ROUTE0_MSG_LEN);
}

int
route0_verify_sodium(const uint8_t *tx, const uint8_t pk[32])
{
    if (tx == NULL || pk == NULL) {
        return -1;
    }
    if (crypto_sign_ed25519_verify_detached(tx + ROUTE0_OFF_SIGNATURE,
                                            tx + ROUTE0_OFF_MESSAGE,
                                            ROUTE0_MSG_LEN, pk) != 0) {
        return -1;
    }
    return 0;
}

int
route0_verify_openssl(const uint8_t *tx, const uint8_t pk[32])
{
    EVP_PKEY *pkey;
    EVP_MD_CTX *md;
    int rc;

    if (tx == NULL || pk == NULL) {
        return -1;
    }
    pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pk, 32);
    if (pkey == NULL) {
        return -1;
    }
    md = EVP_MD_CTX_new();
    if (md == NULL) {
        EVP_PKEY_free(pkey);
        return -1;
    }
    rc = -1;
    if (EVP_DigestVerifyInit(md, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestVerify(md, tx + ROUTE0_OFF_SIGNATURE, 64,
                         tx + ROUTE0_OFF_MESSAGE, ROUTE0_MSG_LEN) == 1) {
        rc = 0;
    }
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return rc;
}

int
route0_ossl_signer_init(route0_ossl_signer_t **out, const uint8_t seed[32])
{
    route0_ossl_signer_t *s;

    if (out == NULL || seed == NULL) {
        return -1;
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return -1;
    }
    s->pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    s->md = EVP_MD_CTX_new();
    if (s->pkey == NULL || s->md == NULL) {
        route0_ossl_signer_free(s);
        return -1;
    }
    if (EVP_DigestSignInit(s->md, NULL, NULL, NULL, s->pkey) != 1) {
        route0_ossl_signer_free(s);
        return -1;
    }
    *out = s;
    return 0;
}

int
route0_ossl_sign(route0_ossl_signer_t *s, uint8_t *tx)
{
    size_t slen = 64;

    if (s == NULL || tx == NULL || s->pkey == NULL || s->md == NULL) {
        return -1;
    }
    if (EVP_DigestSignInit(s->md, NULL, NULL, NULL, s->pkey) != 1) {
        return -1;
    }
    if (EVP_DigestSign(s->md, tx + ROUTE0_OFF_SIGNATURE, &slen,
                       tx + ROUTE0_OFF_MESSAGE, ROUTE0_MSG_LEN) != 1) {
        return -1;
    }
    if (slen != 64u) {
        return -1;
    }
    return 0;
}

void
route0_ossl_signer_free(route0_ossl_signer_t *s)
{
    if (s == NULL) {
        return;
    }
    EVP_MD_CTX_free(s->md);
    EVP_PKEY_free(s->pkey);
    free(s);
}
