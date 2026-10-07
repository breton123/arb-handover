#include "swapix.h"
#include "classify.h"
#include "dlmm.h"
#include "frame.h"
#include "pump.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t DLMM_SWAP1[8] = {
    0xf8, 0xc6, 0x9e, 0x91, 0xe1, 0x75, 0x87, 0xc8
};
static const uint8_t PUMP_BUY[8] = {
    0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
};
static const uint8_t PUMP_SELL[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};
static const uint8_t PUMP_BUY_EQ[8] = {
    0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70
};
static const uint8_t PROG_TOKEN[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93,
    0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91,
    0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9
};
static const uint8_t PROG_TOKENZ[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xee, 0x75, 0x8f, 0xde,
    0x18, 0x42, 0x5d, 0xbc, 0xe4, 0x6c, 0xcd, 0xda,
    0xb6, 0x1a, 0xfc, 0x4d, 0x83, 0xb9, 0x0d, 0x27,
    0xfe, 0xbd, 0xf9, 0x28, 0xd8, 0xa1, 0x8b, 0xfc
};
static const uint8_t PROG_ATA[32] = {
    0x8c, 0x97, 0x25, 0x8f, 0x4e, 0x24, 0x89, 0xf1,
    0xbb, 0x3d, 0x10, 0x29, 0x14, 0x8e, 0x0d, 0x83,
    0x0b, 0x5a, 0x13, 0x99, 0xda, 0xff, 0x10, 0x84,
    0x04, 0x8e, 0x7b, 0xd8, 0xdb, 0xe9, 0xf8, 0x59
};

static int
on_curve(const uint8_t pt[32])
{
    uint8_t yb[32];
    BN_CTX *ctx;
    BIGNUM *y, *p, *d, *y2, *u, *v, *x2, *exp, *res, *pm1;
    int on = 0;

    memcpy(yb, pt, 32);
    yb[31] &= 0x7f;
    ctx = BN_CTX_new();
    if (ctx == NULL) {
        return 0;
    }
    BN_CTX_start(ctx);
    y = BN_CTX_get(ctx);
    p = BN_CTX_get(ctx);
    d = BN_CTX_get(ctx);
    y2 = BN_CTX_get(ctx);
    u = BN_CTX_get(ctx);
    v = BN_CTX_get(ctx);
    x2 = BN_CTX_get(ctx);
    exp = BN_CTX_get(ctx);
    res = BN_CTX_get(ctx);
    pm1 = BN_CTX_get(ctx);
    {
        BIGNUM *pp = p;
        BIGNUM *dd = d;
        if (pm1 == NULL
            || BN_lebin2bn(yb, 32, y) == NULL
            || BN_hex2bn(&pp, "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed") <= 0
            || BN_hex2bn(&dd, "52036cee2b6ffe738cc740797779e89800700a4d4141d8ab75eb4dca135978a3") <= 0) {
            BN_CTX_end(ctx);
            BN_CTX_free(ctx);
            return 0;
        }
    }
    if (BN_cmp(y, p) >= 0) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
        return 0;
    }
    BN_mod_mul(y2, y, y, p, ctx);
    BN_copy(u, y2);
    BN_sub_word(u, 1);
    if (BN_is_negative(u)) {
        BN_add(u, u, p);
    }
    BN_mod_mul(v, d, y2, p, ctx);
    BN_add_word(v, 1);
    BN_mod(v, v, p, ctx);
    if (BN_mod_inverse(x2, v, p, ctx) == NULL) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
        return 0;
    }
    BN_mod_mul(x2, u, x2, p, ctx);
    BN_copy(pm1, p);
    BN_sub_word(pm1, 1);
    BN_rshift1(exp, pm1);
    BN_mod_exp(res, x2, exp, p, ctx);
    on = BN_cmp(res, pm1) != 0;
    BN_CTX_end(ctx);
    BN_CTX_free(ctx);
    return on;
}

static int
ata_addr(const uint8_t owner[32], const uint8_t token_prog[32],
         const uint8_t mint[32], uint8_t out[32])
{
    uint8_t bump;
    uint8_t digest[32];

    for (bump = 255; ; bump--) {
        EVP_MD_CTX *md = EVP_MD_CTX_new();
        if (md == NULL
            || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1
            || EVP_DigestUpdate(md, owner, 32) != 1
            || EVP_DigestUpdate(md, token_prog, 32) != 1
            || EVP_DigestUpdate(md, mint, 32) != 1
            || EVP_DigestUpdate(md, &bump, 1) != 1
            || EVP_DigestUpdate(md, PROG_ATA, 32) != 1
            || EVP_DigestUpdate(md, "ProgramDerivedAddress", 21) != 1
            || EVP_DigestFinal_ex(md, digest, NULL) != 1) {
            EVP_MD_CTX_free(md);
            return -1;
        }
        EVP_MD_CTX_free(md);
        if (!on_curve(digest)) {
            memcpy(out, digest, 32);
            return 0;
        }
        if (bump == 0) {
            break;
        }
    }
    return -1;
}

static int
header_ok(uint8_t nsig, uint8_t ro_signed, uint8_t ro_unsigned, uint16_t nkeys)
{
    if (nsig < 1 || nsig > TX_SIG_MAX) {
        return 0;
    }
    if (ro_signed >= nsig) {
        return 0;
    }
    if (nkeys < nsig || nkeys > TX_KEY_MAX) {
        return 0;
    }
    if ((uint16_t)nsig + (uint16_t)ro_unsigned > nkeys) {
        return 0;
    }
    return 1;
}

static int
key32(const msg_keys_t *mk, uint8_t idx, const uint8_t **out)
{
    if (idx >= mk->nkeys) {
        return -1;
    }
    *out = mk->keys + (uint32_t)idx * 32u;
    return 0;
}

static int
dlmm_dir(const msg_keys_t *mk, const uint8_t *acc, uint16_t nacct,
         uint8_t *swap_for_y)
{
    const uint8_t *user_in;
    const uint8_t *mint_x;
    const uint8_t *mint_y;
    const uint8_t *user;
    const uint8_t *px = PROG_TOKEN;
    const uint8_t *py = PROG_TOKEN;
    uint8_t ata_x[32];
    uint8_t ata_y[32];
    uint8_t try_z;

    if (nacct < 11) {
        return -1;
    }
    if (key32(mk, acc[4], &user_in) != 0
        || key32(mk, acc[6], &mint_x) != 0
        || key32(mk, acc[7], &mint_y) != 0
        || key32(mk, acc[10], &user) != 0) {
        return -1;
    }
    if (nacct > 11 && key32(mk, acc[11], &px) != 0) {
        px = PROG_TOKEN;
    }
    if (nacct > 12 && key32(mk, acc[12], &py) != 0) {
        py = PROG_TOKEN;
    }
    if (ata_addr(user, px, mint_x, ata_x) != 0
        || ata_addr(user, py, mint_y, ata_y) != 0) {
        return -1;
    }
    if (memcmp(user_in, ata_x, 32) == 0) {
        *swap_for_y = 1;
        return 0;
    }
    if (memcmp(user_in, ata_y, 32) == 0) {
        *swap_for_y = 0;
        return 0;
    }
    /* Token-2022 vs Tokenkeg mismatch — try the other program. */
    for (try_z = 0; try_z < 2; try_z++) {
        const uint8_t *alt = (try_z == 0) ? PROG_TOKENZ : PROG_TOKEN;
        if (ata_addr(user, alt, mint_x, ata_x) == 0
            && memcmp(user_in, ata_x, 32) == 0) {
            *swap_for_y = 1;
            return 0;
        }
        if (ata_addr(user, alt, mint_y, ata_y) == 0
            && memcmp(user_in, ata_y, 32) == 0) {
            *swap_for_y = 0;
            return 0;
        }
    }
    return -1;
}

static int
decode_ix(uint8_t proto, const uint8_t *data, uint16_t dlen,
          const msg_keys_t *mk, const uint8_t *acc, uint16_t nacct,
          swapix_t *out)
{
    uint64_t a;
    uint64_t b;

    if (data == NULL || dlen < 24 || nacct < 1) {
        return -1;
    }
    memcpy(&a, data + 8, 8);
    memcpy(&b, data + 16, 8);
    if (a == 0) {
        return -1;
    }
    if (proto == PROTO_DLMM) {
        uint8_t dir;
        if (memcmp(data, DLMM_SWAP2_DISC, 8) == 0) {
            out->variant = 1;
        } else if (memcmp(data, DLMM_SWAP1, 8) == 0) {
            out->variant = 2;
        } else {
            return -1;
        }
        if (dlmm_dir(mk, acc, nacct, &dir) != 0) {
            return -1;
        }
        out->protocol = PROTO_DLMM;
        out->amount_in = a;
        out->min_out = b;
        out->direction = dir;
        return 0;
    }
    if (proto == PROTO_PUMP) {
        if (memcmp(data, PUMP_BUY, 8) == 0) {
            return -1; /* exact-out — unsupported */
        }
        if (memcmp(data, PUMP_SELL, 8) == 0) {
            out->variant = 3;
            out->protocol = PROTO_PUMP;
            out->amount_in = a;
            out->min_out = b;
            out->direction = PUMP_DIR_BASE_TO_QUOTE;
            return 0;
        }
        if (memcmp(data, PUMP_BUY_EQ, 8) == 0) {
            out->variant = 4;
            out->protocol = PROTO_PUMP;
            out->amount_in = a;
            out->min_out = b;
            out->direction = PUMP_DIR_QUOTE_TO_BASE;
            return 0;
        }
        return -1;
    }
    return -1;
}

static int swapix_from_v1(const uint8_t *p, uint16_t len, uint16_t start,
                          const uint8_t *loaded, uint16_t n_loaded, swapix_t *out);

int
swapix_from_msg(const uint8_t *p, uint16_t len, const msg_keys_t *mk,
                swapix_t *out)
{
    uint32_t off;
    uint16_t ninstr;
    uint16_t used;
    uint16_t j;

    if (p == NULL || mk == NULL || out == NULL) {
        return -1;
    }
    if (mk->versioned == FRAME_VER_V1) {
        uint16_t start;
        if (mk->keys_off < TX_V1_HDR_SIZE) {
            return -1;
        }
        start = (uint16_t)(mk->keys_off - TX_V1_HDR_SIZE);
        return swapix_from_v1(p, len, start, mk->keys, mk->nkeys, out);
    }
    memset(out, 0, sizeof(*out));
    off = (uint32_t)mk->keys_off + 32u * (uint32_t)mk->nkeys;
    if (off + TX_BLOCKHASH_SZ > len) {
        return -1;
    }
    off += TX_BLOCKHASH_SZ;
    if (cu16_dec(p + off, (uint16_t)(len - off), &ninstr, &used) != 0) {
        return -1;
    }
    if (ninstr == 0 || ninstr > TX_INSTR_MAX) {
        return -1;
    }
    off += used;
    for (j = 0; j < ninstr; j++) {
        uint8_t prog_idx;
        uint16_t nacct;
        uint16_t dlen;
        uint8_t proto;
        const uint8_t *pkey;

        if (off + 1u > len) {
            return -1;
        }
        prog_idx = p[off];
        off += 1;
        if (prog_idx >= mk->nkeys) {
            return -1;
        }
        if (cu16_dec(p + off, (uint16_t)(len - off), &nacct, &used) != 0) {
            return -1;
        }
        off += used;
        if (nacct > 255 || off + nacct > len) {
            return -1;
        }
        pkey = mk->keys + (uint32_t)prog_idx * 32u;
        proto = 0;
        if (id_eq32(pkey, PROG_DLMM)) {
            proto = PROTO_DLMM;
        } else if (id_eq32(pkey, PROG_PUMP)) {
            proto = PROTO_PUMP;
        }
        if (proto != 0 && nacct >= 1) {
            uint8_t pool_i = p[off];
            const uint8_t *acc = p + off;
            uint32_t data_off;

            off += nacct;
            if (cu16_dec(p + off, (uint16_t)(len - off), &dlen, &used) != 0) {
                return -1;
            }
            off += used;
            if (off + dlen > len) {
                return -1;
            }
            data_off = off;
            off += dlen;
            if (pool_i < mk->nkeys
                && decode_ix(proto, p + data_off, dlen, mk, acc, nacct,
                             out) == 0) {
                memcpy(out->pool, mk->keys + (uint32_t)pool_i * 32u, 32);
                return 0;
            }
            continue;
        }
        off += nacct;
        if (cu16_dec(p + off, (uint16_t)(len - off), &dlen, &used) != 0) {
            return -1;
        }
        off += used;
        if (off + dlen > len) {
            return -1;
        }
        off += dlen;
    }
    return -1;
}

static int
copy_sig(const uint8_t *p, uint16_t len, const msg_keys_t *mk, swapix_t *out)
{
    uint32_t sig_off;

    if (mk->nsig < 1) {
        return -1;
    }
    if ((uint32_t)mk->msg_off < (uint32_t)mk->nsig * TX_SIG_SZ) {
        return -1;
    }
    sig_off = (uint32_t)mk->msg_off - (uint32_t)mk->nsig * TX_SIG_SZ;
    if (sig_off + TX_SIG_SZ > len) {
        return -1;
    }
    memcpy(out->sig, p + sig_off, 64);
    out->have_sig = 1;
    return 0;
}

int
swapix_from_loaded(const uint8_t *p, uint16_t len,
                   uint16_t keys_off, uint16_t n_static,
                   const uint8_t *loaded, uint16_t n_loaded,
                   swapix_t *out)
{
    msg_keys_t mk;
    uint32_t off;
    uint16_t ninstr, used, j;

    if (p == NULL || loaded == NULL || out == NULL || n_loaded == 0) {
        return -1;
    }
    if (len >= 1 && p[0] == TX_V1_PREFIX) {
        return swapix_from_v1(p, len, 0, loaded, n_loaded, out);
    }
    memset(out, 0, sizeof(*out));
    mk.keys = loaded;
    mk.nkeys = n_loaded;
    mk.keys_off = keys_off;
    mk.msg_off = 0;
    mk.nsig = 0;
    mk.versioned = 1;
    off = (uint32_t)keys_off + 32u * (uint32_t)n_static + TX_BLOCKHASH_SZ;
    if (off >= len) {
        return -1;
    }
    if (cu16_dec(p + off, (uint16_t)(len - off), &ninstr, &used) != 0) {
        return -1;
    }
    if (ninstr == 0 || ninstr > TX_INSTR_MAX) {
        return -1;
    }
    off += used;
    for (j = 0; j < ninstr; j++) {
        uint8_t prog_idx;
        uint16_t nacct, dlen;
        uint8_t proto;
        const uint8_t *pkey;

        if (off + 1u > len) {
            return -1;
        }
        prog_idx = p[off++];
        if (prog_idx >= n_loaded) {
            return -1;
        }
        if (cu16_dec(p + off, (uint16_t)(len - off), &nacct, &used) != 0) {
            return -1;
        }
        off += used;
        if (nacct > 255 || off + nacct > len) {
            return -1;
        }
        pkey = loaded + (uint32_t)prog_idx * 32u;
        proto = 0;
        if (id_eq32(pkey, PROG_DLMM)) {
            proto = PROTO_DLMM;
        } else if (id_eq32(pkey, PROG_PUMP)) {
            proto = PROTO_PUMP;
        }
        if (proto != 0 && nacct >= 1) {
            uint8_t pool_i = p[off];
            const uint8_t *acc = p + off;
            uint32_t data_off;
            off += nacct;
            if (cu16_dec(p + off, (uint16_t)(len - off), &dlen, &used) != 0) {
                return -1;
            }
            off += used;
            if (off + dlen > len) {
                return -1;
            }
            data_off = off;
            off += dlen;
            if (pool_i < n_loaded
                && decode_ix(proto, p + data_off, dlen, &mk, acc, nacct,
                             out) == 0) {
                memcpy(out->pool, loaded + (uint32_t)pool_i * 32u, 32);
                return 0;
            }
            continue;
        }
        off += nacct;
        if (cu16_dec(p + off, (uint16_t)(len - off), &dlen, &used) != 0) {
            return -1;
        }
        off += used;
        if (off + dlen > len) {
            return -1;
        }
        off += dlen;
    }
    return -1;
}

static int
swapix_from_v1(const uint8_t *p, uint16_t len, uint16_t start,
               const uint8_t *loaded, uint16_t n_loaded, swapix_t *out)
{
    frame_v1_t L;
    msg_keys_t mk;
    uint8_t i;

    if (frame_v1_layout(p, len, start, &L) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    mk.keys = (loaded != NULL) ? loaded : (p + L.keys_off);
    mk.nkeys = (loaded != NULL) ? n_loaded : L.naddr;
    mk.keys_off = (uint16_t)L.keys_off;
    mk.msg_off = (uint16_t)L.tx_end;
    mk.nsig = L.nsig;
    mk.versioned = FRAME_VER_V1;
    for (i = 0; i < L.ninstr; i++) {
        uint8_t prog, nacct, proto;
        uint16_t dlen;
        uint32_t acc_off, data_off;
        const uint8_t *pkey;

        if (frame_v1_ix(p, &L, i, &prog, &nacct, &dlen, &acc_off, &data_off) != 0) {
            return -1;
        }
        if (prog >= mk.nkeys) {
            return -1;
        }
        pkey = mk.keys + (uint32_t)prog * 32u;
        proto = 0;
        if (id_eq32(pkey, PROG_DLMM)) {
            proto = PROTO_DLMM;
        } else if (id_eq32(pkey, PROG_PUMP)) {
            proto = PROTO_PUMP;
        }
        if (proto != 0 && nacct >= 1) {
            uint8_t pool_i = p[acc_off];
            if (pool_i < mk.nkeys
                && decode_ix(proto, p + data_off, dlen, &mk, p + acc_off, nacct,
                             out) == 0) {
                memcpy(out->pool, mk.keys + (uint32_t)pool_i * 32u, 32);
                return 0;
            }
        }
    }
    return -1;
}

int
swapix_from_tx(const uint8_t *p, uint16_t len, uint16_t start, swapix_t *out)
{
    msg_keys_t mk;
    uint32_t i;
    uint8_t nsig;
    uint8_t b0;
    uint8_t ro_s;
    uint8_t ro_u;
    uint16_t nkeys;
    uint16_t used;
    uint8_t versioned = 0;

    if (p == NULL || out == NULL) {
        return -1;
    }
    if (start < len && p[start] == TX_V1_PREFIX) {
        frame_v1_t L;
        if (swapix_from_v1(p, len, start, NULL, 0, out) != 0) {
            return -1;
        }
        if (frame_v1_layout(p, len, start, &L) != 0) {
            return -1;
        }
        mk.keys = p + L.keys_off;
        mk.nkeys = L.naddr;
        mk.keys_off = (uint16_t)L.keys_off;
        mk.msg_off = (uint16_t)L.tx_end;
        mk.nsig = L.nsig;
        mk.versioned = FRAME_VER_V1;
        return copy_sig(p, len, &mk, out);
    }
    if ((uint32_t)start + 134u > len) {
        return -1;
    }
    i = start;
    if ((p[i] & 0x80u) != 0) {
        return -1;
    }
    nsig = p[i];
    if (nsig < 1 || nsig > TX_SIG_MAX) {
        return -1;
    }
    i += 1u + (uint32_t)nsig * TX_SIG_SZ;
    if (i >= len) {
        return -1;
    }
    b0 = p[i];
    i++;
    if ((b0 & 0x80u) != 0) {
        if ((b0 & 0x7fu) != 0) {
            return -1;
        }
        versioned = 1;
        if (i >= len || p[i] != nsig) {
            return -1;
        }
        i++;
    } else if (b0 != nsig) {
        return -1;
    }
    if (i + 2u > len) {
        return -1;
    }
    ro_s = p[i++];
    ro_u = p[i++];
    if (cu16_dec(p + i, (uint16_t)(len - i), &nkeys, &used) != 0) {
        return -1;
    }
    i += used;
    if (!header_ok(nsig, ro_s, ro_u, nkeys)) {
        return -1;
    }
    if (i + 32u * (uint32_t)nkeys + TX_BLOCKHASH_SZ > len) {
        return -1;
    }
    mk.keys = p + i;
    mk.nkeys = nkeys;
    mk.keys_off = (uint16_t)i;
    mk.msg_off = (uint16_t)(start + 1u + (uint32_t)nsig * TX_SIG_SZ);
    mk.nsig = nsig;
    mk.versioned = versioned;
    if (swapix_from_msg(p, len, &mk, out) != 0) {
        return -1;
    }
    return copy_sig(p, len, &mk, out);
}

int
swapix_from_payload(const uint8_t *p, uint16_t len, swapix_t *out)
{
    uint8_t proto;
    msg_keys_t mk;
    uint16_t prog_off;

    if (p == NULL || out == NULL || len < 134) {
        return -1;
    }
    /*
     * Only recover from a real program id in the key list.
     * Byte-offset txn scan false-positives (random disc + header_ok)
     * were quoting invented N.
     */
    if (find_prog_id(p, len, &prog_off, &proto) != 0) {
        return -1;
    }
    if (msg_keys_from_prog(p, len, prog_off, &mk) != 0) {
        return -1;
    }
    if (swapix_from_msg(p, len, &mk, out) != 0) {
        return -1;
    }
    if (copy_sig(p, len, &mk, out) != 0 || out->amount_in == 0) {
        return -1;
    }
    return 0;
}

int
swapix_from_flat(const uint8_t *keys, uint16_t nkeys,
                 const uint8_t *acc, uint16_t nacct,
                 const uint8_t *data, uint16_t dlen,
                 swapix_t *out)
{
    msg_keys_t mk;
    uint8_t proto;

    if (keys == NULL || acc == NULL || data == NULL || out == NULL
        || nkeys == 0 || nacct == 0 || dlen < 8) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    mk.keys = keys;
    mk.nkeys = nkeys;
    if (memcmp(data, DLMM_SWAP2_DISC, 8) == 0
        || memcmp(data, DLMM_SWAP1, 8) == 0) {
        proto = PROTO_DLMM;
    } else {
        proto = PROTO_PUMP;
    }
    if (decode_ix(proto, data, dlen, &mk, acc, nacct, out) != 0) {
        return -1;
    }
    out->have_sig = 1;
    return 0;
}

int
swapix_to_trigger(const swapix_t *ix, hot_trigger_t *out)
{
    if (ix == NULL || out == NULL || !ix->have_sig) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->protocol = ix->protocol;
    if (ix->protocol == PROTO_DLMM) {
        out->dlmm.amount_in = ix->amount_in;
        out->dlmm.min_amount_out = ix->min_out;
        out->dlmm.swap_for_y = ix->direction;
        return 0;
    }
    if (ix->protocol == PROTO_PUMP) {
        out->pump.amount_in = ix->amount_in;
        out->pump.min_amount_out = ix->min_out;
        out->pump.direction = ix->direction;
        return 0;
    }
    return -1;
}

int
hot_seen_init(hot_seen_t *s)
{
    if (s == NULL) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->cap = SWAPIX_SEEN_CAP;
    s->sig = calloc(s->cap, 64u);
    s->used = calloc(s->cap, 1u);
    if (s->sig == NULL || s->used == NULL) {
        hot_seen_free(s);
        return -1;
    }
    return 0;
}

void
hot_seen_free(hot_seen_t *s)
{
    if (s == NULL) {
        return;
    }
    free(s->sig);
    free(s->used);
    memset(s, 0, sizeof(*s));
}

static uint32_t
sig_hash(const uint8_t sig[64])
{
    uint64_t x;
    memcpy(&x, sig, 8);
    return (uint32_t)(x ^ (x >> 32));
}

int
hot_seen_first(hot_seen_t *s, const uint8_t sig[64])
{
    uint32_t mask;
    uint32_t i;
    uint32_t h;

    if (s == NULL || sig == NULL || s->sig == NULL) {
        return -1;
    }
    mask = s->cap - 1u;
    h = sig_hash(sig) & mask;
    i = h;
    for (;;) {
        if (!s->used[i]) {
            memcpy(s->sig + (size_t)i * 64u, sig, 64);
            s->used[i] = 1;
            s->n++;
            return 1;
        }
        if (memcmp(s->sig + (size_t)i * 64u, sig, 64) == 0) {
            return 0;
        }
        i = (i + 1u) & mask;
        if (i == h) {
            return -1;
        }
    }
}
