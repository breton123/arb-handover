#include "tx/view.h"

#include "shred/txframe.h"

#include <string.h>

static int
sv_get(const uint8_t *p, uint32_t len, uint32_t *off, uint32_t *out)
{
    uint32_t o, v = 0, s = 0, i;

    o = *off;
    for (i = 0; i < 3u; i++) {
        uint8_t b;

        if (o >= len) {
            return TXV_NEED_MORE;
        }
        b = p[o++];
        v |= (uint32_t)(b & 0x7fu) << s;
        if ((b & 0x80u) == 0) {
            if (i != 0 && b == 0) {
                return TXV_BAD;
            }
            *off = o;
            *out = v;
            return TXV_OK;
        }
        if (i == 2u && b > 3u) {
            return TXV_BAD;
        }
        s += 7u;
    }
    return TXV_BAD;
}

static int
decode_v1(const uint8_t *p, uint32_t len, const txframe_t *tf, tx_view_t *o)
{
    uint32_t off, i, pay = 0;
    uint8_t nsig, ninstr, naddr;

    nsig = p[1];
    ninstr = p[40];
    naddr = p[41];
    o->nsig = nsig;
    o->n_static = naddr;
    o->n_keys = naddr;
    o->n_ix = ninstr;
    (void)len;
    if (ninstr > TXV_IX_MAX) {
        return TXV_BAD;
    }
    off = TXF_V1_HDR;
    memcpy(o->key, p + off, (size_t)naddr * 32u);
    off += (uint32_t)naddr * 32u;
    {
        uint32_t mask = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                        | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        uint32_t cfg = 0, m = mask;

        while (m != 0) {
            cfg += m & 1u;
            m >>= 1;
        }
        off += cfg * 4u;
    }
    for (i = 0; i < ninstr; i++) {
        uint8_t nac = p[off + i * 4u + 1u];
        uint16_t dlen = (uint16_t)p[off + i * 4u + 2u]
                        | ((uint16_t)p[off + i * 4u + 3u] << 8);

        pay += (uint32_t)nac + (uint32_t)dlen;
    }
    {
        uint32_t ih = off, body = off + (uint32_t)ninstr * 4u, t;

        for (t = 0; t < ninstr; t++) {
            tx_ix_t *ix = &o->ix[t];
            uint8_t a, nac;
            uint16_t dlen;

            memset(ix, 0, sizeof(*ix));
            ix->prog = p[ih + t * 4u];
            nac = p[ih + t * 4u + 1u];
            dlen = (uint16_t)p[ih + t * 4u + 2u]
                   | ((uint16_t)p[ih + t * 4u + 3u] << 8);
            ix->nacc = (uint8_t)(nac > TXV_ACC_MAX ? TXV_ACC_MAX : nac);
            for (a = 0; a < ix->nacc; a++) {
                ix->acc[a] = p[body + a];
            }
            body += nac;
            ix->dlen = dlen;
            ix->data = p + body;
            body += dlen;
        }
        (void)pay;
        (void)tf;
    }
    memcpy(o->sig, tf->sig, 64);
    return TXV_OK;
}

int
txview_decode(const uint8_t *bytes, uint32_t len, tx_view_t *out)
{
    txframe_t tf;
    uint32_t off = 0, nsig = 0, nkeys = 0, nix = 0, nalt = 0, i;
    int rc, fr;

    if (bytes == NULL || out == NULL) {
        return TXV_BAD;
    }
    memset(out, 0, sizeof(*out));
    out->raw = bytes;
    fr = txframe_parse(bytes, len, &tf);
    if (fr == TXF_NEED_MORE_BYTES) {
        return TXV_NEED_MORE;
    }
    if (fr == TXF_UNSUPPORTED_VERSION) {
        return TXV_UNSUPPORTED;
    }
    if (fr != TXF_OK) {
        return TXV_BAD;
    }
    out->raw_len = tf.tx_len;
    out->version = tf.version;
    memcpy(out->sig, tf.sig, 64);
    if (tf.version == TXF_ENC_V1) {
        return decode_v1(bytes, tf.tx_len, &tf, out);
    }
    rc = sv_get(bytes, len, &off, &nsig);
    if (rc != TXV_OK) {
        return rc;
    }
    out->nsig = (uint8_t)nsig;
    off += nsig * 64u;
    if (tf.version == TXF_ENC_V0) {
        off++;
    }
    if (off + 3u > len) {
        return TXV_NEED_MORE;
    }
    out->hdr_ok = 1;
    out->nro_signed = bytes[off + 1u];
    out->nro_unsigned = bytes[off + 2u];
    off += 3u;
    rc = sv_get(bytes, len, &off, &nkeys);
    if (rc != TXV_OK) {
        return rc;
    }
    if (nkeys == 0 || nkeys > TXV_KEY_MAX) {
        return TXV_BAD;
    }
    out->n_static = (uint16_t)nkeys;
    out->n_keys = (uint16_t)nkeys;
    memcpy(out->key, bytes + off, nkeys * 32u);
    off += nkeys * 32u + 32u;
    rc = sv_get(bytes, len, &off, &nix);
    if (rc != TXV_OK) {
        return rc;
    }
    if (nix > TXV_IX_MAX) {
        return TXV_BAD;
    }
    out->n_ix = (uint16_t)nix;
    for (i = 0; i < nix; i++) {
        uint32_t prog, nac = 0, dlen = 0, a;
        tx_ix_t *ix = &out->ix[i];

        memset(ix, 0, sizeof(*ix));
        if (off >= len) {
            return TXV_NEED_MORE;
        }
        prog = bytes[off++];
        rc = sv_get(bytes, len, &off, &nac);
        if (rc != TXV_OK) {
            return rc;
        }
        ix->prog = (uint8_t)prog;
        ix->nacc = (uint8_t)(nac > TXV_ACC_MAX ? TXV_ACC_MAX : nac);
        for (a = 0; a < nac; a++) {
            if (off >= len) {
                return TXV_NEED_MORE;
            }
            if (a < TXV_ACC_MAX) {
                ix->acc[a] = bytes[off];
            }
            off++;
        }
        rc = sv_get(bytes, len, &off, &dlen);
        if (rc != TXV_OK || off + dlen > len) {
            return (rc != TXV_OK) ? rc : TXV_NEED_MORE;
        }
        ix->dlen = (uint16_t)dlen;
        ix->data = bytes + off;
        off += dlen;
    }
    if (tf.version == TXF_ENC_V0) {
        rc = sv_get(bytes, len, &off, &nalt);
        if (rc != TXV_OK) {
            return rc;
        }
        if (nalt > TXV_ALT_MAX) {
            return TXV_BAD;
        }
        out->n_alt = (uint8_t)nalt;
        for (i = 0; i < nalt; i++) {
            uint32_t nw = 0, nr = 0, j;
            tx_alt_t *a = &out->alt[i];

            memset(a, 0, sizeof(*a));
            if (off + 32u > len) {
                return TXV_NEED_MORE;
            }
            memcpy(a->table, bytes + off, 32);
            off += 32u;
            rc = sv_get(bytes, len, &off, &nw);
            if (rc != TXV_OK) {
                return rc;
            }
            if (nw > TXV_IDX_MAX) {
                return TXV_BAD;
            }
            a->nw = (uint8_t)nw;
            for (j = 0; j < nw; j++) {
                if (off >= len) {
                    return TXV_NEED_MORE;
                }
                a->widx[j] = bytes[off++];
            }
            rc = sv_get(bytes, len, &off, &nr);
            if (rc != TXV_OK) {
                return rc;
            }
            if (nr > TXV_IDX_MAX) {
                return TXV_BAD;
            }
            a->nr = (uint8_t)nr;
            for (j = 0; j < nr; j++) {
                if (off >= len) {
                    return TXV_NEED_MORE;
                }
                a->ridx[j] = bytes[off++];
            }
        }
    }
    return TXV_OK;
}
