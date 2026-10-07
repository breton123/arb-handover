#include "ingress/decode.h"

#include <string.h>

static const uint8_t PUMP_PK[32] = {
    0x0c, 0x14, 0xde, 0xfc, 0x82, 0x5e, 0xc6, 0x76, 0x94, 0x25, 0x08, 0x18,
    0xbb, 0x65, 0x40, 0x65, 0xf4, 0x29, 0x8d, 0x31, 0x56, 0xd5, 0x71, 0xb4,
    0xd4, 0xf8, 0x09, 0x0c, 0x18, 0xe9, 0xa8, 0x63
};
static const uint8_t DLMM_PK[32] = {
    0x04, 0xe9, 0xe1, 0x2f, 0xbc, 0x84, 0xe8, 0x26, 0xc9, 0x32, 0xcc, 0xe9,
    0xe2, 0x64, 0x0c, 0xce, 0x15, 0x59, 0x0c, 0x1c, 0x62, 0x73, 0xb0, 0x92,
    0x57, 0x08, 0xba, 0x3b, 0x85, 0x20, 0xb0, 0xbc
};
static const uint8_t TOK_PK[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1, 0x46,
    0xce, 0xeb, 0x79, 0xac, 0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91,
    0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9
};
static const uint8_t T22_PK[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xee, 0x75, 0x8f, 0xde, 0x18, 0x42, 0x5d, 0xbc,
    0xe4, 0x6c, 0xcd, 0xda, 0xb6, 0x1a, 0xfc, 0x4d, 0x83, 0xb9, 0x0d, 0x27,
    0xfe, 0xbd, 0xf9, 0x28, 0xd8, 0xa1, 0x8b, 0xfc
};
static const uint8_t SYS_PK[32] = { 0 };
static const uint8_t ATA_PK[32] = {
    0x8c, 0x97, 0x25, 0x8f, 0x4e, 0x24, 0x89, 0xf1, 0xbb, 0x3d, 0x10, 0x29,
    0x14, 0x8e, 0x0d, 0x83, 0x0b, 0x5a, 0x13, 0x99, 0xda, 0xff, 0x10, 0x84,
    0x04, 0x8e, 0x7b, 0xd8, 0xdb, 0xe9, 0xf8, 0x59
};

static const uint8_t DISC_SELL[8] = { 0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad };
static const uint8_t DISC_BUYEQ[8] = { 0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70 };
static const uint8_t DISC_BUYOUT[8] = { 0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea };
static const uint8_t DISC_XFER[8] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t DISC_SYNC[8] = { 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t DISC_CLOSE[8] = { 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

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

static uint64_t
u64le(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static int
eq32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32) == 0;
}

static void
ix_none(ordered_ix_t *ix)
{
    ix->pool_id = STATE_ACCT_NONE;
    ix->src_token = STATE_ACCT_NONE;
    ix->dst_token = STATE_ACCT_NONE;
    ix->vault_base = STATE_ACCT_NONE;
    ix->vault_quote = STATE_ACCT_NONE;
    ix->fee_proto = STATE_ACCT_NONE;
    ix->fee_creator = STATE_ACCT_NONE;
    ix->src_sys = STATE_ACCT_NONE;
    ix->dst_sys = STATE_ACCT_NONE;
}

static void
fill_pump(ordered_ix_t *ix, const uint8_t *data, uint32_t dlen)
{
    if (dlen < 8) {
        ix->kind = IX_KIND_OTHER;
        return;
    }
    if (memcmp(data, DISC_SELL, 8) == 0) {
        ix->kind = IX_KIND_PUMP_SELL;
        ix->direction = 1;
    } else if (memcmp(data, DISC_BUYEQ, 8) == 0) {
        ix->kind = IX_KIND_PUMP_BUY_EQ;
        ix->direction = 0;
    } else if (memcmp(data, DISC_BUYOUT, 8) == 0) {
        ix->kind = IX_KIND_PUMP_BUY;
        ix->direction = 0;
    } else {
        ix->kind = IX_KIND_OTHER;
        return;
    }
    if (dlen >= 16) {
        ix->amount_in = u64le(data + 8);
    }
    if (dlen >= 24) {
        ix->min_out = u64le(data + 16);
    }
}

int
ingress_decode_tx(const uint8_t *bytes, uint32_t len, uint64_t slot,
                  ordered_tx_t *out)
{
    uint32_t off = 0, nsig = 0, nkeys = 0, nix = 0, nalt = 0, i;
    uint8_t enc = TX_ENC_LEGACY;

    if (bytes == NULL || out == NULL) {
        return DEC_INVALID;
    }
    ordered_tx_clear(out);
    out->slot = slot;
    out->raw = bytes;
    out->raw_len = len;
    if (sv_get(bytes, len, &off, &nsig) != 0 || nsig == 0 || nsig > 16) {
        return DEC_INCOMPLETE;
    }
    if (off + nsig * 64u > len) {
        return DEC_INCOMPLETE;
    }
    memcpy(out->sig, bytes + off, STATE_SIG_LEN);
    off += nsig * 64u;
    if (off >= len) {
        return DEC_INCOMPLETE;
    }
    if ((bytes[off] & 0x80u) != 0) {
        uint8_t ver = (uint8_t)(bytes[off] & 0x7fu);

        off++;
        enc = (ver == 0) ? TX_ENC_V0 : TX_ENC_V1;
    }
    out->encoding = enc;
    if (off + 3u > len) {
        return DEC_INCOMPLETE;
    }
    out->hdr_ok = 1;
    out->nsig = (uint8_t)nsig;
    out->nro_signed = bytes[off + 1u];
    out->nro_unsigned = bytes[off + 2u];
    off += 3u; /* num_required / readonly signed / readonly unsigned */
    if (sv_get(bytes, len, &off, &nkeys) != 0 || nkeys == 0
        || nkeys > STATE_KEY_MAX) {
        return DEC_INCOMPLETE;
    }
    if (off + nkeys * 32u + 32u > len) {
        return DEC_INCOMPLETE;
    }
    out->n_keys = (uint16_t)nkeys;
    out->n_static = (uint16_t)nkeys;
    memcpy(out->key, bytes + off, nkeys * 32u);
    off += nkeys * 32u + 32u; /* blockhash */
    if (sv_get(bytes, len, &off, &nix) != 0) {
        return DEC_INCOMPLETE;
    }
    if (nix > STATE_IX_MAX) {
        nix = STATE_IX_MAX;
    }
    for (i = 0; i < nix; i++) {
        uint32_t prog = 0, nac = 0, dlen = 0, a;
        ordered_ix_t *ix = &out->ix[i];
        const uint8_t *pk;

        memset(ix, 0, sizeof(*ix));
        ix_none(ix);
        ix->relevant = 1;
        ix->prog = (uint8_t)prog;
        if (off >= len) {
            return DEC_INCOMPLETE;
        }
        prog = bytes[off++];
        if (sv_get(bytes, len, &off, &nac) != 0) {
            return DEC_INCOMPLETE;
        }
        if (nac > STATE_ACC_MAX) {
            nac = STATE_ACC_MAX;
        }
        out->acc_n[i] = (uint8_t)nac;
        for (a = 0; a < nac; a++) {
            if (off >= len) {
                return DEC_INCOMPLETE;
            }
            out->acc_ix[i][a] = bytes[off++];
        }
        if (sv_get(bytes, len, &off, &dlen) != 0 || off + dlen > len) {
            return DEC_INCOMPLETE;
        }
        if (prog >= nkeys) {
            return DEC_INVALID;
        }
        pk = out->key[prog];
        if (eq32(pk, PUMP_PK)) {
            ix->proto = PROTO_PUMP;
            fill_pump(ix, bytes + off, dlen);
        } else if (eq32(pk, DLMM_PK)) {
            ix->proto = PROTO_DLMM;
            ix->kind = IX_KIND_DLMM_SWAP;
        } else if (eq32(pk, TOK_PK)) {
            ix->proto = PROTO_TOKEN;
            if (dlen >= 1 && bytes[off] == 3) {
                ix->kind = IX_KIND_TOKEN_XFER;
                if (dlen >= 9) {
                    ix->amount_in = u64le(bytes + off + 1);
                }
            } else if (dlen >= 1 && bytes[off] == 17) {
                ix->kind = IX_KIND_TOKEN_SYNC;
            } else if (dlen >= 1 && bytes[off] == 9) {
                ix->kind = IX_KIND_TOKEN_CLOSE;
            } else {
                ix->kind = IX_KIND_OTHER;
                ix->proto = PROTO_UNKNOWN;
            }
            (void)DISC_XFER;
            (void)DISC_SYNC;
            (void)DISC_CLOSE;
        } else if (eq32(pk, T22_PK)) {
            ix->proto = PROTO_TOKEN;
            ix->kind = IX_KIND_OTHER;
        } else if (eq32(pk, SYS_PK)) {
            ix->proto = PROTO_SYSTEM;
            ix->kind = (dlen >= 4 && bytes[off] == 2) ? IX_KIND_SYS_TRANSFER
                                                     : IX_KIND_OTHER;
            if (ix->kind == IX_KIND_SYS_TRANSFER && dlen >= 12) {
                ix->amount_in = u64le(bytes + off + 4);
            }
        } else if (eq32(pk, ATA_PK)) {
            ix->proto = PROTO_ATA;
            ix->kind = IX_KIND_ATA_CREATE;
        } else {
            ix->proto = PROTO_UNKNOWN;
            ix->kind = IX_KIND_OTHER;
        }
        off += dlen;
    }
    out->n_ix = (uint16_t)nix;
    if (enc == TX_ENC_V0) {
        if (sv_get(bytes, len, &off, &nalt) != 0) {
            return DEC_INCOMPLETE;
        }
        if (nalt != 0) {
            out->raw_len = off;
            return DEC_ALT;
        }
    }
    out->raw_len = off;
    return DEC_OK;
}
