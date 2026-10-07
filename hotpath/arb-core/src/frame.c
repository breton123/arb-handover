#include "frame.h"
#include "classify.h"

#include <string.h>

enum { R_BAD = -1, R_NEED = 1, R_OK = 0 };

static int
cu16(const uint8_t *p, uint32_t avail, uint16_t *val, uint16_t *used)
{
    if (avail < 1) {
        return R_NEED;
    }
    if ((p[0] & 0x80u) == 0) {
        *val = p[0];
        *used = 1;
        return R_OK;
    }
    if (avail < 2) {
        return R_NEED;
    }
    if ((p[1] & 0x80u) == 0) {
        if (p[1] == 0) {
            return R_BAD;
        }
        *val = (uint16_t)((p[0] & 0x7fu) | ((uint16_t)p[1] << 7));
        *used = 2;
        return R_OK;
    }
    if (avail < 3) {
        return R_NEED;
    }
    if ((p[2] & 0xfcu) != 0 || p[2] == 0) {
        return R_BAD;
    }
    *val = (uint16_t)((p[0] & 0x7fu)
        | ((uint16_t)(p[1] & 0x7fu) << 7)
        | ((uint16_t)p[2] << 14));
    *used = 3;
    return R_OK;
}

static int
header_ok(uint8_t nsig, uint8_t ro_s, uint8_t ro_u, uint16_t nkeys)
{
    if (nsig < 1 || nsig > TX_SIG_MAX) {
        return 0;
    }
    if (ro_s >= nsig) {
        return 0;
    }
    if (nkeys < nsig || nkeys > TX_KEY_MAX) {
        return 0;
    }
    if ((uint16_t)nsig + (uint16_t)ro_u > nkeys) {
        return 0;
    }
    return 1;
}

static int
is_dex(const uint8_t *key)
{
    return id_eq32(key, PROG_DLMM) || id_eq32(key, PROG_PUMP);
}

static uint32_t
pop32(uint32_t x)
{
    uint32_t n = 0;
    while (x != 0) {
        n += x & 1u;
        x >>= 1;
    }
    return n;
}

static int
v1_dups(const uint8_t *keys, uint8_t n)
{
    uint8_t i, j;
    for (i = 0; i < n; i++) {
        for (j = (uint8_t)(i + 1u); j < n; j++) {
            if (memcmp(keys + (uint32_t)i * TX_KEY_SZ,
                       keys + (uint32_t)j * TX_KEY_SZ, TX_KEY_SZ) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

int
frame_v1_layout(const uint8_t *p, uint32_t len, uint32_t start, frame_v1_t *out)
{
    uint32_t off, mask, cfg_n, pay, i, proj;
    uint8_t nsig, ro_s, ro_u, ninstr, naddr;

    if (p == NULL || out == NULL) {
        return R_BAD;
    }
    memset(out, 0, sizeof(*out));
    if (start >= len) {
        return R_NEED;
    }
    if (p[start] != TX_V1_PREFIX) {
        return R_BAD;
    }
    if (start + TX_V1_HDR_SIZE > len) {
        return R_NEED;
    }
    nsig = p[start + 1u];
    ro_s = p[start + 2u];
    ro_u = p[start + 3u];
    mask = (uint32_t)p[start + 4u]
        | ((uint32_t)p[start + 5u] << 8)
        | ((uint32_t)p[start + 6u] << 16)
        | ((uint32_t)p[start + 7u] << 24);
    if ((mask & ~TX_V1_CFG_KNOWN) != 0) {
        return R_BAD;
    }
    if ((mask & 3u) == 1u || (mask & 3u) == 2u) {
        return R_BAD;
    }
    ninstr = p[start + 40u];
    naddr = p[start + 41u];
    if (ninstr == 0 || ninstr > TX_INSTR_MAX) {
        return R_BAD;
    }
    if (!header_ok(nsig, ro_s, ro_u, naddr)) {
        return R_BAD;
    }
    off = start + TX_V1_HDR_SIZE;
    if (off + (uint32_t)naddr * TX_KEY_SZ > len) {
        return R_NEED;
    }
    if (v1_dups(p + off, naddr)) {
        return R_BAD;
    }
    out->nsig = nsig;
    out->ninstr = ninstr;
    out->naddr = naddr;
    out->mask = mask;
    out->keys_off = off;
    off += (uint32_t)naddr * TX_KEY_SZ;
    cfg_n = pop32(mask) * 4u;
    out->cfg_off = off;
    if (off + cfg_n > len) {
        return R_NEED;
    }
    off += cfg_n;
    out->hdr_off = off;
    if (off + (uint32_t)ninstr * 4u > len) {
        return R_NEED;
    }
    pay = 0;
    for (i = 0; i < ninstr; i++) {
        uint8_t prog = p[off + i * 4u];
        uint8_t nacct = p[off + i * 4u + 1u];
        uint16_t dlen = (uint16_t)p[off + i * 4u + 2u]
            | ((uint16_t)p[off + i * 4u + 3u] << 8);
        if (prog >= naddr) {
            return R_BAD;
        }
        pay += (uint32_t)nacct + (uint32_t)dlen;
    }
    off += (uint32_t)ninstr * 4u;
    out->pay_off = off;
    proj = (off - start) + pay + (uint32_t)nsig * TX_SIG_SZ;
    if (proj > TX_V1_MAX) {
        return R_BAD;
    }
    if (off + pay > len) {
        return R_NEED;
    }
    {
        uint32_t cursor = off;
        for (i = 0; i < ninstr; i++) {
            uint8_t nacct = p[out->hdr_off + i * 4u + 1u];
            uint16_t dlen = (uint16_t)p[out->hdr_off + i * 4u + 2u]
                | ((uint16_t)p[out->hdr_off + i * 4u + 3u] << 8);
            uint8_t a;
            if (cursor + nacct + dlen > len) {
                return R_NEED;
            }
            for (a = 0; a < nacct; a++) {
                if (p[cursor + a] >= naddr) {
                    return R_BAD;
                }
            }
            cursor += (uint32_t)nacct + (uint32_t)dlen;
        }
        off = cursor;
    }
    out->sig_off = off;
    if (off + (uint32_t)nsig * TX_SIG_SZ > len) {
        return R_NEED;
    }
    off += (uint32_t)nsig * TX_SIG_SZ;
    out->tx_end = off;
    return R_OK;
}

int
frame_v1_ix(const uint8_t *p, const frame_v1_t *L, uint8_t i,
            uint8_t *prog, uint8_t *nacct, uint16_t *dlen,
            uint32_t *acc_off, uint32_t *data_off)
{
    uint32_t cursor;
    uint8_t j;

    if (p == NULL || L == NULL || i >= L->ninstr) {
        return -1;
    }
    cursor = L->pay_off;
    for (j = 0; j < i; j++) {
        uint8_t na = p[L->hdr_off + (uint32_t)j * 4u + 1u];
        uint16_t dl = (uint16_t)p[L->hdr_off + (uint32_t)j * 4u + 2u]
            | ((uint16_t)p[L->hdr_off + (uint32_t)j * 4u + 3u] << 8);
        cursor += (uint32_t)na + (uint32_t)dl;
    }
    if (prog != NULL) {
        *prog = p[L->hdr_off + (uint32_t)i * 4u];
    }
    if (nacct != NULL) {
        *nacct = p[L->hdr_off + (uint32_t)i * 4u + 1u];
    }
    if (dlen != NULL) {
        *dlen = (uint16_t)p[L->hdr_off + (uint32_t)i * 4u + 2u]
            | ((uint16_t)p[L->hdr_off + (uint32_t)i * 4u + 3u] << 8);
    }
    if (acc_off != NULL) {
        *acc_off = cursor;
    }
    if (data_off != NULL) {
        *data_off = cursor + p[L->hdr_off + (uint32_t)i * 4u + 1u];
    }
    return 0;
}

static int
frame_parse_v1(const uint8_t *p, uint32_t len, uint32_t start, frame_hit_t *out,
               int require_dex)
{
    frame_v1_t L;
    int rc;
    uint8_t have_dex = 0, k;

    if (out == NULL) {
        return R_BAD;
    }
    memset(out, 0, sizeof(*out));
    out->tx_off = start;
    rc = frame_v1_layout(p, len, start, &L);
    if (rc != R_OK) {
        return rc;
    }
    for (k = 0; k < L.naddr; k++) {
        if (is_dex(p + L.keys_off + (uint32_t)k * TX_KEY_SZ)) {
            have_dex = 1;
            break;
        }
    }
    if (require_dex && !have_dex) {
        return R_BAD;
    }
    out->klass = FRAME_FRAMED;
    out->nsig = L.nsig;
    out->versioned = FRAME_VER_V1;
    out->have_dex = have_dex;
    out->nkeys = L.naddr;
    out->ninstr = L.ninstr;
    out->tx_end = L.tx_end;
    return R_OK;
}

static int
frame_parse(const uint8_t *p, uint32_t len, uint32_t start, frame_hit_t *out,
            int require_dex)
{
    uint32_t off, keys_off = 0;
    uint16_t nsig, used, nkeys, ninstr, nlut, i, a;
    uint8_t b0, ro_s, ro_u, versioned = 0, have_dex = 0;
    int rc;

    if (p == NULL || out == NULL) {
        return R_BAD;
    }
    memset(out, 0, sizeof(*out));
    out->tx_off = start;
    if ((uint32_t)start >= len) {
        return R_NEED;
    }
    off = start;
    rc = cu16(p + off, len - off, &nsig, &used);
    if (rc != R_OK) {
        return rc;
    }
    if (nsig < 1 || nsig > TX_SIG_MAX) {
        return R_BAD;
    }
    off += used;
    if (off + (uint32_t)nsig * TX_SIG_SZ > len) {
        return R_NEED;
    }
    off += (uint32_t)nsig * TX_SIG_SZ;
    if (off >= len) {
        return R_NEED;
    }
    b0 = p[off++];
    if ((b0 & 0x80u) != 0) {
        if ((b0 & 0x7fu) != 0) {
            return R_BAD;
        }
        versioned = 1;
        if (off >= len) {
            return R_NEED;
        }
        if (p[off] != (uint8_t)nsig) {
            return R_BAD;
        }
        off++;
    } else if (b0 != (uint8_t)nsig) {
        return R_BAD;
    }
    if (off + 2u > len) {
        return R_NEED;
    }
    ro_s = p[off++];
    ro_u = p[off++];
    rc = cu16(p + off, len - off, &nkeys, &used);
    if (rc != R_OK) {
        return rc;
    }
    if (!header_ok((uint8_t)nsig, ro_s, ro_u, nkeys)) {
        return R_BAD;
    }
    off += used;
    if (off + (uint32_t)nkeys * TX_KEY_SZ + TX_BLOCKHASH_SZ > len) {
        return R_NEED;
    }
    {
        keys_off = off;
        off += (uint32_t)nkeys * TX_KEY_SZ + TX_BLOCKHASH_SZ;
        rc = cu16(p + off, len - off, &ninstr, &used);
        if (rc != R_OK) {
            return rc;
        }
        if (ninstr == 0 || ninstr > TX_INSTR_MAX) {
            return R_BAD;
        }
        off += used;
        for (i = 0; i < ninstr; i++) {
            uint8_t prog_idx;
            uint16_t nacct, dlen;
            if (off >= len) {
                return R_NEED;
            }
            prog_idx = p[off++];
            if (versioned) {
                /* Loaded-key index. Validated after LUT parse. */
            } else if (prog_idx >= nkeys) {
                return R_BAD;
            }
            if (!versioned && is_dex(p + keys_off + (uint32_t)prog_idx * TX_KEY_SZ)) {
                have_dex = 1;
            } else if (versioned && prog_idx < nkeys
                       && is_dex(p + keys_off + (uint32_t)prog_idx * TX_KEY_SZ)) {
                have_dex = 1;
            }
            rc = cu16(p + off, len - off, &nacct, &used);
            if (rc != R_OK) {
                return rc;
            }
            off += used;
            if (nacct > 255) {
                return R_BAD;
            }
            if (off + nacct > len) {
                return R_NEED;
            }
            for (a = 0; a < nacct; a++) {
                if (!versioned && p[off + a] >= nkeys) {
                    return R_BAD;
                }
            }
            off += nacct;
            rc = cu16(p + off, len - off, &dlen, &used);
            if (rc != R_OK) {
                return rc;
            }
            off += used;
            if (off + dlen > len) {
                return R_NEED;
            }
            off += dlen;
        }
    }
    if (versioned) {
        rc = cu16(p + off, len - off, &nlut, &used);
        if (rc != R_OK) {
            return rc;
        }
        if (nlut > 32) {
            return R_BAD;
        }
        off += used;
        for (i = 0; i < nlut; i++) {
            uint16_t nw, nr;
            if (off + 32u > len) {
                return R_NEED;
            }
            off += 32u;
            rc = cu16(p + off, len - off, &nw, &used);
            if (rc != R_OK) {
                return rc;
            }
            off += used;
            if (off + nw > len) {
                return R_NEED;
            }
            off += nw;
            rc = cu16(p + off, len - off, &nr, &used);
            if (rc != R_OK) {
                return rc;
            }
            off += used;
            if (off + nr > len) {
                return R_NEED;
            }
            off += nr;
        }
    }
    if (!have_dex) {
        uint16_t k;
        for (k = 0; k < nkeys; k++) {
            if (is_dex(p + keys_off + (uint32_t)k * TX_KEY_SZ)) {
                have_dex = 1;
                break;
            }
        }
    }
    if (require_dex && !have_dex) {
        return R_BAD;
    }
    out->klass = FRAME_FRAMED;
    out->nsig = (uint8_t)nsig;
    out->versioned = versioned;
    out->have_dex = have_dex;
    out->nkeys = nkeys;
    out->ninstr = ninstr;
    out->tx_end = off;
    return R_OK;
}

int
frame_try_at(const uint8_t *p, uint32_t len, uint32_t start, frame_hit_t *out)
{
    if (p != NULL && start < len && p[start] == TX_V1_PREFIX) {
        return frame_parse_v1(p, len, start, out, 1);
    }
    return frame_parse(p, len, start, out, 1);
}

int
frame_try_any(const uint8_t *p, uint32_t len, uint32_t start, frame_hit_t *out)
{
    if (p != NULL && start < len && p[start] == TX_V1_PREFIX) {
        return frame_parse_v1(p, len, start, out, 0);
    }
    return frame_parse(p, len, start, out, 0);
}

static int
frame_v1_from_sig0(const uint8_t *p, uint32_t len, uint32_t sig_pos,
                   int require_dex, frame_hit_t *out)
{
    uint32_t s, lo;
    int best = R_BAD;
    frame_hit_t hit;

    if (p == NULL || out == NULL || sig_pos + TX_SIG_SZ > len) {
        return R_BAD;
    }
    lo = (sig_pos + TX_SIG_SZ > TX_V1_MAX) ? (sig_pos + TX_SIG_SZ - TX_V1_MAX) : 0;
    for (s = lo; s <= sig_pos; s++) {
        int rc;
        uint32_t sig0;
        if (p[s] != TX_V1_PREFIX) {
            continue;
        }
        rc = frame_parse_v1(p, len, s, &hit, require_dex);
        if (rc == R_NEED) {
            if (best != R_OK) {
                best = R_NEED;
            }
            continue;
        }
        if (rc != R_OK || hit.nsig < 1) {
            continue;
        }
        sig0 = hit.tx_end - (uint32_t)hit.nsig * TX_SIG_SZ;
        if (sig0 != sig_pos) {
            continue;
        }
        *out = hit;
        return R_OK;
    }
    return best;
}

int
frame_around_sig(const uint8_t *p, uint32_t len, const uint8_t sig[64],
                 frame_hit_t *out)
{
    uint32_t pos;
    int best = R_BAD;
    int seen = 0;
    frame_hit_t hit, best_hit;

    if (p == NULL || sig == NULL || out == NULL) {
        return FRAME_INVALID;
    }
    memset(out, 0, sizeof(*out));
    memset(&best_hit, 0, sizeof(best_hit));
    if (len < 65) {
        out->klass = FRAME_INCOMPLETE;
        return FRAME_INCOMPLETE;
    }
    for (pos = 0; pos + 64u <= len; pos++) {
        uint8_t back;
        int rc;
        if (memcmp(p + pos, sig, 64) != 0) {
            continue;
        }
        seen = 1;
        for (back = 1; back <= 3 && back <= pos; back++) {
            uint32_t start = pos - back;
            uint16_t nsig, used;
            rc = cu16(p + start, len - start, &nsig, &used);
            if (rc == R_NEED) {
                if (best != R_OK) {
                    best = R_NEED;
                }
                continue;
            }
            if (rc != R_OK || used != back) {
                continue;
            }
            if (nsig < 1 || nsig > TX_SIG_MAX) {
                continue;
            }
            /* signatures[0] must be exactly the candidate */
            if (start + used + 64u > len) {
                if (best != R_OK) {
                    best = R_NEED;
                }
                continue;
            }
            if (memcmp(p + start + used, sig, 64) != 0) {
                continue;
            }
            rc = frame_try_at(p, len, start, &hit);
            if (rc == R_OK) {
                best = R_OK;
                best_hit = hit;
                best_hit.klass = FRAME_FRAMED;
                goto done;
            }
            if (rc == R_NEED && best != R_OK) {
                best = R_NEED;
                best_hit = hit;
                best_hit.klass = FRAME_INCOMPLETE;
            }
        }
        rc = frame_v1_from_sig0(p, len, pos, 1, &hit);
        if (rc == R_OK) {
            best = R_OK;
            best_hit = hit;
            best_hit.klass = FRAME_FRAMED;
            goto done;
        }
        if (rc == R_NEED && best != R_OK) {
            best = R_NEED;
            best_hit = hit;
            best_hit.klass = FRAME_INCOMPLETE;
        }
        /* sig present but no legal shortvec immediately before it */
        if (best == R_BAD && pos >= 1) {
            best = R_BAD;
        }
    }
done:
    if (best == R_OK) {
        *out = best_hit;
        out->klass = FRAME_FRAMED;
        return FRAME_FRAMED;
    }
    if (!seen || best == R_NEED) {
        *out = best_hit;
        out->klass = FRAME_INCOMPLETE;
        return FRAME_INCOMPLETE;
    }
    out->klass = FRAME_INVALID;
    return FRAME_INVALID;
}

int
frame_around_sig_any(const uint8_t *p, uint32_t len, const uint8_t sig[64],
                     frame_hit_t *out)
{
    uint32_t pos;
    int best = R_BAD;
    int seen = 0;
    frame_hit_t hit, best_hit;

    if (p == NULL || sig == NULL || out == NULL) {
        return FRAME_INVALID;
    }
    memset(out, 0, sizeof(*out));
    memset(&best_hit, 0, sizeof(best_hit));
    if (len < 65) {
        out->klass = FRAME_INCOMPLETE;
        return FRAME_INCOMPLETE;
    }
    for (pos = 0; pos + 64u <= len; pos++) {
        uint8_t back;
        int rc;
        if (memcmp(p + pos, sig, 64) != 0) {
            continue;
        }
        seen = 1;
        for (back = 1; back <= 3 && back <= pos; back++) {
            uint32_t start = pos - back;
            uint16_t nsig, used;
            rc = cu16(p + start, len - start, &nsig, &used);
            if (rc != R_OK || used != back) {
                continue;
            }
            if (nsig < 1 || nsig > TX_SIG_MAX) {
                continue;
            }
            if (start + used + 64u > len) {
                if (best != R_OK) {
                    best = R_NEED;
                }
                continue;
            }
            if (memcmp(p + start + used, sig, 64) != 0) {
                continue;
            }
            rc = frame_try_any(p, len, start, &hit);
            if (rc == R_OK) {
                *out = hit;
                out->klass = FRAME_FRAMED;
                return FRAME_FRAMED;
            }
            if (rc == R_NEED && best != R_OK) {
                best = R_NEED;
                best_hit = hit;
                best_hit.klass = FRAME_INCOMPLETE;
            }
        }
        rc = frame_v1_from_sig0(p, len, pos, 0, &hit);
        if (rc == R_OK) {
            *out = hit;
            out->klass = FRAME_FRAMED;
            return FRAME_FRAMED;
        }
        if (rc == R_NEED && best != R_OK) {
            best = R_NEED;
            best_hit = hit;
            best_hit.klass = FRAME_INCOMPLETE;
        }
    }
    if (!seen || best == R_NEED) {
        *out = best_hit;
        out->klass = FRAME_INCOMPLETE;
        return FRAME_INCOMPLETE;
    }
    out->klass = FRAME_INVALID;
    return FRAME_INVALID;
}
