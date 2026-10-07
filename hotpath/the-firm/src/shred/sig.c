#include "shred/sig.h"

#include <string.h>

#if defined(__linux__)
#include <openssl/evp.h>
#define FIRM_ED25519 1
#endif

#if defined(FIRM_ED25519)
static int
sv_get(const uint8_t *p, uint32_t len, uint32_t *off, uint32_t *out)
{
    uint32_t o, v = 0, s = 0;

    o = *off;
    do {
        uint8_t b;

        if (o >= len) {
            return -1;
        }
        b = p[o++];
        v |= (uint32_t)(b & 0x7fu) << s;
        s += 7u;
        if ((b & 0x80u) == 0) {
            *off = o;
            *out = v;
            return 0;
        }
    } while (s <= 28u);
    return -1;
}
#endif

int
shred_tx_sig_verify(const uint8_t *tx, uint32_t len)
{
#if !defined(FIRM_ED25519)
    (void)tx;
    (void)len;
    return -1;
#else
    uint32_t off = 0, nsig = 0, nkeys = 0;
    const uint8_t *sig, *msg, *pk;
    uint32_t msglen;
    EVP_PKEY *pkey;
    EVP_MD_CTX *ctx;
    int rc;

    if (tx == NULL || len < 64u + 4u) {
        return -1;
    }
    if (sv_get(tx, len, &off, &nsig) != 0 || nsig == 0 || nsig > 16u) {
        return -1;
    }
    if (off + nsig * 64u >= len) {
        return -1;
    }
    sig = tx + off;
    off += nsig * 64u;
    msg = tx + off;
    msglen = len - off;
    if (msglen == 0) {
        return -1;
    }
    if ((msg[0] & 0x80u) != 0) {
        off++;
    }
    if (off + 3u >= len) {
        return -1;
    }
    off += 3u;
    if (sv_get(tx, len, &off, &nkeys) != 0 || nkeys == 0) {
        return -1;
    }
    if (off + 32u > len) {
        return -1;
    }
    pk = tx + off;
    pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pk, 32);
    if (pkey == NULL) {
        return -1;
    }
    ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        EVP_PKEY_free(pkey);
        return -1;
    }
    rc = 0;
    if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1
        && EVP_DigestVerify(ctx, sig, 64, msg, msglen) == 1) {
        rc = 1;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
#endif
}
