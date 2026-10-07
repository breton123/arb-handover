#include "smoke_tx.h"

#include "tx_template.h"

#include <string.h>

static const uint8_t MEMO_PROG[32] = {
    5, 74, 83, 90, 153, 41, 33, 6, 77, 36, 232, 113, 96, 218, 56, 124,
    124, 53, 181, 221, 188, 146, 187, 129, 228, 31, 168, 64, 65, 5, 68, 141
};

static const uint8_t SYSVAR_RECENT[32] = {
    6, 167, 213, 23, 25, 44, 86, 142, 224, 138, 132, 95, 115, 210, 151, 136,
    207, 3, 92, 49, 69, 178, 26, 179, 68, 216, 6, 46, 169, 64, 0, 0
};

static const uint8_t SYSVAR_RENT[32] = {
    6, 167, 213, 23, 25, 44, 92, 81, 33, 140, 201, 76, 61, 74, 241, 127,
    88, 218, 238, 8, 155, 161, 253, 68, 227, 219, 217, 138, 0, 0, 0, 0
};

static const uint8_t COMPUTE_BUDGET[32] = ROUTE0_COMPUTE_BUDGET_ID;
static const uint8_t SYSTEM_PROG[32] = { 0 };

static int
put_u8(uint8_t *b, uint16_t *c, uint16_t cap, uint8_t v)
{
    if ((uint32_t)*c + 1u > cap) {
        return -1;
    }
    b[(*c)++] = v;
    return 0;
}

static int
put_bytes(uint8_t *b, uint16_t *c, uint16_t cap, const uint8_t *p, uint16_t n)
{
    if ((uint32_t)*c + n > cap) {
        return -1;
    }
    memcpy(b + *c, p, n);
    *c = (uint16_t)(*c + n);
    return 0;
}

static int
put_cu16(uint8_t *b, uint16_t *c, uint16_t cap, uint16_t v)
{
    if (v >= 128u) {
        return -1;
    }
    return put_u8(b, c, cap, (uint8_t)v);
}

static int
ix_cu_limit(uint8_t *b, uint16_t *c, uint16_t cap, uint8_t prog, uint32_t units)
{
    if (put_u8(b, c, cap, prog) != 0 || put_cu16(b, c, cap, 0) != 0 ||
        put_cu16(b, c, cap, 5) != 0 || put_u8(b, c, cap, 2) != 0) {
        return -1;
    }
    return put_bytes(b, c, cap, (const uint8_t *)&units, 4);
}

static int
ix_cu_price(uint8_t *b, uint16_t *c, uint16_t cap, uint8_t prog, uint64_t price)
{
    if (put_u8(b, c, cap, prog) != 0 || put_cu16(b, c, cap, 0) != 0 ||
        put_cu16(b, c, cap, 9) != 0 || put_u8(b, c, cap, 3) != 0) {
        return -1;
    }
    return put_bytes(b, c, cap, (const uint8_t *)&price, 8);
}

static int
begin_tx(smoke_tx_t *out, uint8_t nsig)
{
    uint16_t c = 0;
    uint8_t i;

    memset(out, 0, sizeof(*out));
    if (put_cu16(out->bytes, &c, SMOKE_TX_MAX, nsig) != 0) {
        return -1;
    }
    for (i = 0; i < nsig; i++) {
        if ((uint32_t)c + 64u > SMOKE_TX_MAX) {
            return -1;
        }
        memset(out->bytes + c, 0, 64);
        c = (uint16_t)(c + 64u);
    }
    out->msg_off = c;
    out->nsig = nsig;
    return 0;
}

static void
finish_msg(smoke_tx_t *out, uint16_t c)
{
    out->len = c;
    out->msg_len = (uint16_t)(c - out->msg_off);
}

int
smoke_build_memo(const uint8_t payer[32], const uint8_t blockhash[32],
                 uint64_t cu_price, smoke_tx_t *out)
{
    uint16_t c;
    uint8_t memo_len = (uint8_t)(sizeof(SMOKE_MEMO) - 1u);

    if (begin_tx(out, 1) != 0) {
        return -1;
    }
    c = out->msg_off;
    /* 1 signer, 0 ro-signed, 2 ro-unsigned (memo + compute) */
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 0) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 2) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 3) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, payer, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, MEMO_PROG, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, COMPUTE_BUDGET, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, blockhash, 32) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 3) != 0) {
        return -1;
    }
    if (ix_cu_limit(out->bytes, &c, SMOKE_TX_MAX, 2, 50000u) != 0 ||
        ix_cu_price(out->bytes, &c, SMOKE_TX_MAX, 2, cu_price) != 0) {
        return -1;
    }
    /* memo: prog 1, acc [0], data */
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 0) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, memo_len) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX,
                  (const uint8_t *)SMOKE_MEMO, memo_len) != 0) {
        return -1;
    }
    finish_msg(out, c);
    return 0;
}

int
smoke_build_create_nonce(const uint8_t payer[32],
                         const uint8_t nonce_pk[32],
                         const uint8_t blockhash[32],
                         uint64_t rent, uint64_t cu_price,
                         smoke_tx_t *out)
{
    uint16_t c;
    uint8_t create[52];
    uint8_t init[36];
    uint64_t space = 80;

    memset(create, 0, sizeof(create));
    memcpy(create + 4, &rent, 8);
    memcpy(create + 12, &space, 8);
    /* owner = system program, already zero */
    memset(init, 0, sizeof(init));
    init[0] = 6;
    memcpy(init + 4, payer, 32);

    if (begin_tx(out, 2) != 0) {
        return -1;
    }
    c = out->msg_off;
    /* 2 signers, 0 ro-signed, 4 ro (recent, rent, compute, system) */
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 2) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 0) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 4) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 6) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, payer, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, nonce_pk, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, SYSVAR_RECENT, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, SYSVAR_RENT, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, COMPUTE_BUDGET, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, SYSTEM_PROG, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, blockhash, 32) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 4) != 0) {
        return -1;
    }
    if (ix_cu_limit(out->bytes, &c, SMOKE_TX_MAX, 4, 50000u) != 0 ||
        ix_cu_price(out->bytes, &c, SMOKE_TX_MAX, 4, cu_price) != 0) {
        return -1;
    }
    /* createAccount: system=5, acc payer=0 nonce=1 */
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 5) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 2) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 0) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 52) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, create, 52) != 0) {
        return -1;
    }
    /* initializeNonce: system=5, nonce=1, recent=2, rent=3 */
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 5) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 3) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 2) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 3) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 36) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, init, 36) != 0) {
        return -1;
    }
    finish_msg(out, c);
    return 0;
}

int
smoke_build_nonce_memo(const uint8_t payer[32],
                       const uint8_t nonce_acc[32],
                       const uint8_t nonce_hash[32],
                       uint64_t cu_price, smoke_tx_t *out)
{
    uint16_t c;
    uint8_t advance[4];
    uint8_t memo_len = (uint8_t)(sizeof(SMOKE_MEMO) - 1u);

    memset(advance, 0, sizeof(advance));
    advance[0] = 4;
    if (begin_tx(out, 1) != 0) {
        return -1;
    }
    c = out->msg_off;
    /* signer writable, nonce writable, 3 ro: sysvar, memo, compute, system */
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 0) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 4) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 6) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, payer, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, nonce_acc, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, SYSVAR_RECENT, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, MEMO_PROG, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, COMPUTE_BUDGET, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, SYSTEM_PROG, 32) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, nonce_hash, 32) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 4) != 0) {
        return -1;
    }
    /* AdvanceNonce MUST be first. */
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 5) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 3) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 2) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 0) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 4) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX, advance, 4) != 0) {
        return -1;
    }
    if (ix_cu_limit(out->bytes, &c, SMOKE_TX_MAX, 4, 50000u) != 0 ||
        ix_cu_price(out->bytes, &c, SMOKE_TX_MAX, 4, cu_price) != 0) {
        return -1;
    }
    if (put_u8(out->bytes, &c, SMOKE_TX_MAX, 3) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, 1) != 0 ||
        put_u8(out->bytes, &c, SMOKE_TX_MAX, 0) != 0 ||
        put_cu16(out->bytes, &c, SMOKE_TX_MAX, memo_len) != 0 ||
        put_bytes(out->bytes, &c, SMOKE_TX_MAX,
                  (const uint8_t *)SMOKE_MEMO, memo_len) != 0) {
        return -1;
    }
    finish_msg(out, c);
    return 0;
}

int
smoke_sign(const route0_signer_t *signers, uint8_t nsig, smoke_tx_t *tx)
{
    uint8_t i;

    if (signers == NULL || tx == NULL || nsig == 0 || nsig != tx->nsig) {
        return -1;
    }
    for (i = 0; i < nsig; i++) {
        if (exec_sign(&signers[i], tx->bytes + 1u + (uint16_t)i * 64u,
                      tx->bytes + tx->msg_off, tx->msg_len) != 0) {
            return -1;
        }
    }
    return 0;
}
