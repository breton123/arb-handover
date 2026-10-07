#include "tx.h"
#include "classify.h"
#include "frame.h"

#include <string.h>

int
cu16_dec(const uint8_t *p, uint16_t avail, uint16_t *val, uint16_t *used)
{
    if (p == NULL || avail < 1) {
        return -1;
    }
    if ((p[0] & 0x80u) == 0) {
        *val = p[0];
        *used = 1;
        return 0;
    }
    if (avail < 2) {
        return -1;
    }
    if ((p[1] & 0x80u) == 0) {
        if (p[1] == 0) {
            return -1;
        }
        *val = (uint16_t)((uint16_t)(p[0] & 0x7fu) | ((uint16_t)p[1] << 7));
        *used = 2;
        return 0;
    }
    if (avail < 3 || (p[2] & 0xfcu) != 0 || p[2] == 0) {
        return -1;
    }
    *val = (uint16_t)((uint16_t)(p[0] & 0x7fu)
        | ((uint16_t)(p[1] & 0x7fu) << 7)
        | ((uint16_t)p[2] << 14));
    *used = 3;
    return 0;
}

int
find_next_prog_id(const uint8_t *p, uint16_t len, uint16_t from,
                  uint16_t *off, uint8_t *proto)
{
    uint16_t i;
    uint16_t last;

    if (p == NULL || len < 32) {
        return -1;
    }
    last = (uint16_t)(len - 32);
    if (from > last) {
        return -1;
    }
    for (i = from; i <= last; i++) {
        if (id_eq32(p + i, PROG_DLMM)) {
            *off = i;
            *proto = PROTO_DLMM;
            return 0;
        }
        if (id_eq32(p + i, PROG_PUMP)) {
            *off = i;
            *proto = PROTO_PUMP;
            return 0;
        }
    }
    return -1;
}

int
find_prog_id(const uint8_t *p, uint16_t len, uint16_t *off, uint8_t *proto)
{
    return find_next_prog_id(p, len, 0, off, proto);
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

int
msg_keys_from_prog(const uint8_t *p, uint16_t len, uint16_t prog_off,
                   msg_keys_t *out)
{
    uint16_t k;

    if (p == NULL || out == NULL || (uint32_t)prog_off + 32u > len) {
        return -1;
    }
    {
        uint16_t vk;
        for (vk = 0; vk < TX_KEY_MAX; vk++) {
            uint32_t start;
            frame_v1_t L;
            if ((uint32_t)prog_off < TX_V1_HDR_SIZE + (uint32_t)vk * TX_KEY_SZ) {
                break;
            }
            start = (uint32_t)prog_off - TX_V1_HDR_SIZE - (uint32_t)vk * TX_KEY_SZ;
            if (p[start] != TX_V1_PREFIX) {
                continue;
            }
            if (frame_v1_layout(p, len, start, &L) != 0) {
                continue;
            }
            if (L.keys_off + (uint32_t)vk * TX_KEY_SZ != (uint32_t)prog_off) {
                continue;
            }
            if (vk >= L.naddr || L.tx_end > 65535u) {
                continue;
            }
            out->keys = p + L.keys_off;
            out->nkeys = L.naddr;
            out->keys_off = (uint16_t)L.keys_off;
            out->msg_off = (uint16_t)L.tx_end;
            out->nsig = L.nsig;
            out->versioned = FRAME_VER_V1;
            return 0;
        }
    }
    for (k = 1; k < TX_KEY_MAX; k++) {
        uint32_t start32;
        uint16_t start;
        uint16_t w;

        if (prog_off < (uint16_t)(k * 32u)) {
            break;
        }
        start32 = (uint32_t)prog_off - (uint32_t)k * 32u;
        start = (uint16_t)start32;
        for (w = 1; w <= 3; w++) {
            uint16_t nkeys;
            uint16_t used;
            uint32_t keys_end;
            uint8_t nsig;
            uint8_t ro_s;
            uint8_t ro_u;
            int versioned = 0;

            if (start < w) {
                continue;
            }
            if (cu16_dec(p + start - w, w, &nkeys, &used) != 0 || used != w) {
                continue;
            }
            if (nkeys < (uint16_t)(k + 1u) || nkeys > TX_KEY_MAX) {
                continue;
            }
            keys_end = (uint32_t)start + 32u * (uint32_t)nkeys;
            if (keys_end > len) {
                continue;
            }

            if (start >= (uint16_t)(w + 3u)) {
                const uint8_t *h = p + start - w - 3;
                nsig = h[0];
                ro_s = h[1];
                ro_u = h[2];
                if (header_ok(nsig, ro_s, ro_u, nkeys)) {
                    out->keys = p + start;
                    out->nkeys = nkeys;
                    out->keys_off = start;
                    out->msg_off = (uint16_t)(start - w - 3);
                    out->nsig = nsig;
                    out->versioned = 0;
                    return 0;
                }
            }
            if (start >= (uint16_t)(w + 4u) && p[start - w - 4] == 0x80u) {
                const uint8_t *h = p + start - w - 3;
                nsig = h[0];
                ro_s = h[1];
                ro_u = h[2];
                versioned = 1;
                if (header_ok(nsig, ro_s, ro_u, nkeys)) {
                    out->keys = p + start;
                    out->nkeys = nkeys;
                    out->keys_off = start;
                    out->msg_off = (uint16_t)(start - w - 4);
                    out->nsig = nsig;
                    out->versioned = (uint8_t)versioned;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int
key_is_dex(const uint8_t *key, uint8_t *proto)
{
    if (id_eq32(key, PROG_DLMM)) {
        *proto = PROTO_DLMM;
        return 1;
    }
    if (id_eq32(key, PROG_PUMP)) {
        *proto = PROTO_PUMP;
        return 1;
    }
    return 0;
}

int
msg_first_dex_pool(const uint8_t *p, uint16_t len, const msg_keys_t *mk,
                   pool_cand_t *out)
{
    uint32_t off;
    uint16_t ninstr;
    uint16_t used;
    uint16_t j;

    if (p == NULL || mk == NULL || out == NULL) {
        return -1;
    }
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

        if (off + 1u > len) {
            return -1;
        }
        prog_idx = p[off];
        off += 1;
        if (prog_idx == 0 || prog_idx >= mk->nkeys) {
            return -1;
        }
        if (cu16_dec(p + off, (uint16_t)(len - off), &nacct, &used) != 0) {
            return -1;
        }
        off += used;
        if (nacct > 255 || off + nacct > len) {
            return -1;
        }
        if (key_is_dex(mk->keys + (uint32_t)prog_idx * 32u, &proto)
            && nacct >= 1) {
            uint8_t pool_idx = p[off];
            if (pool_idx < mk->nkeys) {
                out->protocol = proto;
                out->key = mk->keys + (uint32_t)pool_idx * 32u;
                return 0;
            }
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

int
txn_dex_at(const uint8_t *p, uint16_t len, uint16_t start,
           msg_keys_t *mk, pool_cand_t *cand)
{
    uint32_t i;
    uint8_t nsig;
    uint8_t b0;
    uint8_t ro_s;
    uint8_t ro_u;
    uint16_t nkeys;
    uint16_t used;
    uint8_t versioned = 0;

    if (p == NULL || mk == NULL || cand == NULL || (uint32_t)start + 134u > len) {
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
    mk->keys = p + i;
    mk->nkeys = nkeys;
    mk->keys_off = (uint16_t)i;
    mk->msg_off = (uint16_t)(start + 1u + (uint32_t)nsig * TX_SIG_SZ);
    mk->nsig = nsig;
    mk->versioned = versioned;
    return msg_first_dex_pool(p, len, mk, cand);
}
