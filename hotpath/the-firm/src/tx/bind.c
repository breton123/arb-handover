#include "tx/bind.h"

#include <string.h>

static const uint8_t PUMP_AMM[32] = {
    0x0c, 0x14, 0xde, 0xfc, 0x82, 0x5e, 0xc6, 0x76, 0x94, 0x25, 0x08, 0x18,
    0xbb, 0x65, 0x40, 0x65, 0xf4, 0x29, 0x8d, 0x31, 0x56, 0xd5, 0x71, 0xb4,
    0xd4, 0xf8, 0x09, 0x0c, 0x18, 0xe9, 0xa8, 0x63
};
static const uint8_t TOK_PK[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1, 0x46,
    0xce, 0xeb, 0x79, 0xac, 0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91,
    0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9
};
static const uint8_t SYS_PK[32] = { 0 };
static const uint8_t ATA_PK[32] = {
    0x8c, 0x97, 0x25, 0x8f, 0x4e, 0x24, 0x89, 0xf1, 0xbb, 0x3d, 0x10, 0x29,
    0x14, 0x8e, 0x0d, 0x83, 0x0b, 0x5a, 0x13, 0x99, 0xda, 0xff, 0x10, 0x84,
    0x04, 0x8e, 0x7b, 0xd8, 0xdb, 0xe9, 0xf8, 0x59
};
static const uint8_t DISC_SELL[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};
static const uint8_t DISC_BUYEQ[8] = {
    0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70
};
static const uint8_t DISC_BUYOUT[8] = {
    0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
};

static int
eq32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32) == 0;
}

static uint64_t
u64le(const uint8_t *p)
{
    uint64_t v;

    memcpy(&v, p, 8);
    return v;
}

static void
ix_ids_none(ordered_ix_t *ix)
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

static const uint8_t *
key_at(const tx_view_t *v, uint8_t idx)
{
    if (idx >= v->n_keys) {
        return NULL;
    }
    return v->key[idx];
}

static void
fill_pump(ordered_ix_t *ix, const uint8_t *data, uint32_t dlen)
{
    if (dlen < 8u) {
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
    if (dlen >= 16u) {
        ix->amount_in = u64le(data + 8);
    }
    if (dlen >= 24u) {
        ix->min_out = u64le(data + 16);
    }
}

int
txview_bind(const tx_view_t *v, uint64_t slot, ordered_tx_t *out)
{
    uint16_t i, nk;

    if (v == NULL || out == NULL) {
        return -1;
    }
    ordered_tx_clear(out);
    out->slot = slot;
    out->raw = v->raw;
    out->raw_len = v->raw_len;
    memcpy(out->sig, v->sig, STATE_SIG_LEN);
    out->encoding = v->version;
    nk = v->n_keys;
    if (nk > STATE_KEY_MAX) {
        return -1;
    }
    out->n_keys = nk;
    out->nsig = v->nsig;
    out->nro_signed = v->nro_signed;
    out->nro_unsigned = v->nro_unsigned;
    out->n_static = v->n_static;
    out->hdr_ok = v->hdr_ok;
    {
        uint8_t a;
        uint16_t lw = 0;

        for (a = 0; a < v->n_alt; a++) {
            lw = (uint16_t)(lw + v->alt[a].nw);
        }
        out->n_lut_w = lw;
    }
    if (nk != 0) {
        memcpy(out->key, v->key, (size_t)nk * 32u);
    }
    if (v->n_ix > STATE_IX_MAX) {
        return -1;
    }
    out->n_ix = v->n_ix;
    for (i = 0; i < v->n_ix; i++) {
        const tx_ix_t *src = &v->ix[i];
        ordered_ix_t *ix = &out->ix[i];
        const uint8_t *pk;
        uint8_t a, nac;

        memset(ix, 0, sizeof(*ix));
        ix_ids_none(ix);
        ix->relevant = 1;
        ix->prog = src->prog;
        nac = src->nacc;
        if (nac > STATE_ACC_MAX) {
            nac = STATE_ACC_MAX;
        }
        out->acc_n[i] = nac;
        for (a = 0; a < nac; a++) {
            out->acc_ix[i][a] = src->acc[a];
        }
        pk = key_at(v, src->prog);
        if (pk == NULL) {
            return -1;
        }
        if (eq32(pk, PUMP_AMM)) {
            ix->proto = PROTO_PUMP;
            fill_pump(ix, src->data, src->dlen);
        } else if (eq32(pk, TOK_PK)) {
            ix->proto = PROTO_TOKEN;
            if (src->dlen >= 1u && src->data[0] == 3) {
                ix->kind = IX_KIND_TOKEN_XFER;
                if (src->dlen >= 9u) {
                    ix->amount_in = u64le(src->data + 1);
                }
            } else if (src->dlen >= 1u && src->data[0] == 17) {
                ix->kind = IX_KIND_TOKEN_SYNC;
            } else if (src->dlen >= 1u && src->data[0] == 9) {
                ix->kind = IX_KIND_TOKEN_CLOSE;
            } else {
                ix->kind = IX_KIND_OTHER;
                ix->proto = PROTO_UNKNOWN;
            }
        } else if (eq32(pk, SYS_PK)) {
            ix->proto = PROTO_SYSTEM;
            ix->kind = (src->dlen >= 4u && src->data[0] == 2)
                           ? IX_KIND_SYS_TRANSFER
                           : IX_KIND_OTHER;
            if (ix->kind == IX_KIND_SYS_TRANSFER && src->dlen >= 12u) {
                ix->amount_in = u64le(src->data + 4);
            }
        } else if (eq32(pk, ATA_PK)) {
            ix->proto = PROTO_ATA;
            ix->kind = IX_KIND_ATA_CREATE;
        } else {
            ix->proto = PROTO_UNKNOWN;
            ix->kind = IX_KIND_OTHER;
        }
    }
    return 0;
}
