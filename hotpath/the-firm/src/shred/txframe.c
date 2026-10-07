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
            return TXF_NEED_MORE_BYTES;
        }
        b = p[o++];
        v |= (uint32_t)(b & 0x7fu) << s;
        if ((b & 0x80u) == 0) {
            if (i != 0 && b == 0) {
                return TXF_BAD_SHORTVEC;
            }
            *off = o;
            *out = v;
            return TXF_OK;
        }
        if (i == 2u && b > 3u) {
            return TXF_BAD_SHORTVEC;
        }
        s += 7u;
    }
    return TXF_BAD_SHORTVEC;
}

static void
prefix8(txframe_t *o, const uint8_t *p, uint32_t off, uint32_t len)
{
    uint32_t n = 8u;

    memset(o->msg_prefix, 0, 8);
    if (off >= len) {
        return;
    }
    if (len - off < n) {
        n = len - off;
    }
    memcpy(o->msg_prefix, p + off, n);
}

const char *
txframe_name(int rc)
{
    switch (rc) {
    case TXF_OK:
        return "OK";
    case TXF_NEED_MORE_BYTES:
        return "NEED_MORE_BYTES";
    case TXF_BAD_SHORTVEC:
        return "BAD_SHORTVEC";
    case TXF_BAD_SIGNATURE_SECTION:
        return "BAD_SIGNATURE_SECTION";
    case TXF_UNSUPPORTED_VERSION:
        return "UNSUPPORTED_VERSION";
    case TXF_BAD_MESSAGE:
        return "BAD_MESSAGE";
    default:
        return "OTHER";
    }
}

static int
parse_v1(const uint8_t *p, uint32_t len, txframe_t *o)
{
    uint32_t mask, cfg, pay = 0, off, i;
    uint8_t nsig, ninstr, naddr, rs, ru;

    if (len < TXF_V1_HDR) {
        return TXF_NEED_MORE_BYTES;
    }
    nsig = p[1];
    rs = p[2];
    ru = p[3];
    mask = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16)
           | ((uint32_t)p[7] << 24);
    if ((mask & ~31u) != 0 || (mask & 3u) == 1u || (mask & 3u) == 2u) {
        return TXF_BAD_MESSAGE;
    }
    ninstr = p[40];
    naddr = p[41];
    if (nsig < 1u || nsig > 12u || ninstr == 0 || ninstr > 64u || naddr < nsig
        || naddr > 64u || rs >= nsig || (uint32_t)nsig + ru > naddr) {
        return TXF_BAD_MESSAGE;
    }
    off = TXF_V1_HDR;
    if (off + (uint32_t)naddr * 32u > len) {
        return TXF_NEED_MORE_BYTES;
    }
    off += (uint32_t)naddr * 32u;
    cfg = 0;
    {
        uint32_t m = mask;

        while (m != 0) {
            cfg += m & 1u;
            m >>= 1;
        }
    }
    cfg *= 4u;
    if (off + cfg + (uint32_t)ninstr * 4u > len) {
        return TXF_NEED_MORE_BYTES;
    }
    off += cfg;
    for (i = 0; i < ninstr; i++) {
        uint8_t prog = p[off + i * 4u];
        uint8_t nac = p[off + i * 4u + 1u];
        uint16_t dlen = (uint16_t)p[off + i * 4u + 2u]
                        | ((uint16_t)p[off + i * 4u + 3u] << 8);

        if (prog >= naddr) {
            return TXF_BAD_MESSAGE;
        }
        pay += (uint32_t)nac + (uint32_t)dlen;
    }
    off += (uint32_t)ninstr * 4u;
    if (off + pay + (uint32_t)nsig * 64u > TXF_V1_MAX) {
        return TXF_BAD_MESSAGE;
    }
    if (off + pay + (uint32_t)nsig * 64u > len) {
        return TXF_NEED_MORE_BYTES;
    }
    off += pay;
    o->version = TXF_ENC_V1;
    o->nsig = nsig;
    o->nkeys = naddr;
    o->nix = ninstr;
    o->message_off = 0;
    memcpy(o->sig, p + off, 64);
    prefix8(o, p, 0, len);
    o->tx_len = off + (uint32_t)nsig * 64u;
    return TXF_OK;
}

static int
parse_legacy_v0(const uint8_t *p, uint32_t len, txframe_t *o)
{
    uint32_t off = 0, nsig = 0, nkeys = 0, nix = 0, nalt = 0, i;
    uint8_t enc = TXF_ENC_LEGACY;
    int rc;

    rc = sv_get(p, len, &off, &nsig);
    if (rc != TXF_OK) {
        return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                         : TXF_BAD_SHORTVEC;
    }
    if (nsig < 1u || nsig > TXF_SIG_MAX) {
        return TXF_BAD_SIGNATURE_SECTION;
    }
    if (off + nsig * 64u > len) {
        return TXF_NEED_MORE_BYTES;
    }
    memcpy(o->sig, p + off, 64);
    off += nsig * 64u;
    if (off >= len) {
        return TXF_NEED_MORE_BYTES;
    }
    o->message_off = off;
    prefix8(o, p, off, len);
    if ((p[off] & 0x80u) != 0) {
        uint8_t ver = (uint8_t)(p[off] & 0x7fu);

        if (ver != 0) {
            return TXF_UNSUPPORTED_VERSION;
        }
        enc = TXF_ENC_V0;
        off++;
        if (off >= len) {
            return TXF_NEED_MORE_BYTES;
        }
    }
    if (off + 3u > len) {
        return TXF_NEED_MORE_BYTES;
    }
    if (p[off] != (uint8_t)nsig) {
        return TXF_BAD_MESSAGE;
    }
    off += 3u;
    rc = sv_get(p, len, &off, &nkeys);
    if (rc != TXF_OK) {
        return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                         : TXF_BAD_SHORTVEC;
    }
    if (nkeys < nsig || nkeys > TXF_KEY_MAX) {
        return TXF_BAD_MESSAGE;
    }
    if (off + nkeys * 32u + 32u > len) {
        return TXF_NEED_MORE_BYTES;
    }
    off += nkeys * 32u + 32u;
    rc = sv_get(p, len, &off, &nix);
    if (rc != TXF_OK) {
        return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                         : TXF_BAD_SHORTVEC;
    }
    if (nix > TXF_IX_MAX) {
        return TXF_BAD_MESSAGE;
    }
    for (i = 0; i < nix; i++) {
        uint32_t nac = 0, dlen = 0;

        if (off >= len) {
            return TXF_NEED_MORE_BYTES;
        }
        off++;
        rc = sv_get(p, len, &off, &nac);
        if (rc != TXF_OK) {
            return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                             : TXF_BAD_SHORTVEC;
        }
        if (off + nac > len) {
            return TXF_NEED_MORE_BYTES;
        }
        off += nac;
        rc = sv_get(p, len, &off, &dlen);
        if (rc != TXF_OK) {
            return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                             : TXF_BAD_SHORTVEC;
        }
        if (off + dlen > len) {
            return TXF_NEED_MORE_BYTES;
        }
        off += dlen;
    }
    if (enc == TXF_ENC_V0) {
        rc = sv_get(p, len, &off, &nalt);
        if (rc != TXF_OK) {
            return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                             : TXF_BAD_SHORTVEC;
        }
        if (nalt > TXF_KEY_MAX) {
            return TXF_BAD_MESSAGE;
        }
        for (i = 0; i < nalt; i++) {
            uint32_t nw = 0, nr = 0;

            if (off + 32u > len) {
                return TXF_NEED_MORE_BYTES;
            }
            off += 32u;
            rc = sv_get(p, len, &off, &nw);
            if (rc != TXF_OK) {
                return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                                 : TXF_BAD_SHORTVEC;
            }
            if (off + nw > len) {
                return TXF_NEED_MORE_BYTES;
            }
            off += nw;
            rc = sv_get(p, len, &off, &nr);
            if (rc != TXF_OK) {
                return rc == TXF_NEED_MORE_BYTES ? TXF_NEED_MORE_BYTES
                                                 : TXF_BAD_SHORTVEC;
            }
            if (off + nr > len) {
                return TXF_NEED_MORE_BYTES;
            }
            off += nr;
        }
    }
    if (off > TXF_V0_MAX) {
        return TXF_BAD_MESSAGE;
    }
    o->version = enc;
    o->nsig = nsig;
    o->nkeys = nkeys;
    o->nix = nix;
    o->tx_len = off;
    return TXF_OK;
}

int
txframe_parse(const uint8_t *bytes, uint32_t len, txframe_t *out)
{
    if (bytes == NULL || out == NULL) {
        return TXF_BAD_MESSAGE;
    }
    memset(out, 0, sizeof(*out));
    if (len == 0) {
        return TXF_NEED_MORE_BYTES;
    }
    if (bytes[0] == TXF_V1_PREFIX) {
        return parse_v1(bytes, len, out);
    }
    if ((bytes[0] & 0x80u) != 0) {
        return TXF_UNSUPPORTED_VERSION;
    }
    return parse_legacy_v0(bytes, len, out);
}
