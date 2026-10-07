#include "wire/classify.h"

#include <string.h>

static const uint8_t PUMP_AMM[32] = {
    0x0c, 0x14, 0xde, 0xfc, 0x82, 0x5e, 0xc6, 0x76, 0x94, 0x25, 0x08, 0x18,
    0xbb, 0x65, 0x40, 0x65, 0xf4, 0x29, 0x8d, 0x31, 0x56, 0xd5, 0x71, 0xb4,
    0xd4, 0xf8, 0x09, 0x0c, 0x18, 0xe9, 0xa8, 0x63
};
static const uint8_t PUMP_BOND[32] = {
    0x01, 0x56, 0xe0, 0xf6, 0x93, 0x66, 0x5a, 0xcf, 0x44, 0xdb, 0x15, 0x68,
    0xbf, 0x17, 0x5b, 0xaa, 0x51, 0x89, 0xcb, 0x97, 0xf5, 0xd2, 0xff, 0x3b,
    0x65, 0x5d, 0x2b, 0xb6, 0xfd, 0x6d, 0x18, 0xb0
};
static const uint8_t DLMM[32] = {
    0x04, 0xe9, 0xe1, 0x2f, 0xbc, 0x84, 0xe8, 0x26, 0xc9, 0x32, 0xcc, 0xe9,
    0xe2, 0x64, 0x0c, 0xce, 0x15, 0x59, 0x0c, 0x1c, 0x62, 0x73, 0xb0, 0x92,
    0x57, 0x08, 0xba, 0x3b, 0x85, 0x20, 0xb0, 0xbc
};
/* JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4 */
static const uint8_t JUP6[32] = {
    0x04, 0x79, 0xd5, 0x5b, 0xf2, 0x31, 0xc0, 0x6e, 0xee, 0x74, 0xc5, 0x6e,
    0xce, 0x68, 0x15, 0x07, 0xfd, 0xb1, 0xb2, 0xde, 0xa3, 0xf4, 0x8e, 0x51,
    0x02, 0xb1, 0xcd, 0xa2, 0x56, 0xbc, 0x13, 0x8f
};

static int
eq32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32) == 0;
}

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

static int
is_pump(const uint8_t *k)
{
    return eq32(k, PUMP_AMM) || eq32(k, PUMP_BOND);
}

static int
is_dlmm(const uint8_t *k)
{
    return eq32(k, DLMM);
}

static int
is_router(const uint8_t *k)
{
    return eq32(k, JUP6);
}

static int
is_watched(const uint8_t *k)
{
    return is_pump(k) || is_dlmm(k);
}

void
wire_lut_tab_clear(wire_lut_tab_t *tab)
{
    if (tab != NULL) {
        memset(tab, 0, sizeof(*tab));
    }
}

int
wire_lut_put(wire_lut_tab_t *tab, const uint8_t key[32], const uint8_t *addrs,
             uint32_t n)
{
    uint32_t i;

    if (tab == NULL || key == NULL || (n != 0 && addrs == NULL) || n > 256u) {
        return -1;
    }
    for (i = 0; i < tab->n; i++) {
        if (tab->t[i].used && eq32(tab->t[i].key, key)) {
            tab->t[i].n = (uint16_t)n;
            memcpy(tab->t[i].addr, addrs, (size_t)n * 32u);
            return 0;
        }
    }
    if (tab->n >= WIRE_LUT_MAX) {
        return -1;
    }
    i = tab->n++;
    tab->t[i].used = 1;
    tab->t[i].n = (uint16_t)n;
    memcpy(tab->t[i].key, key, 32);
    if (n != 0) {
        memcpy(tab->t[i].addr, addrs, (size_t)n * 32u);
    }
    return 0;
}

static const wire_lut_t *
lut_find(const wire_lut_tab_t *tab, const uint8_t key[32])
{
    uint32_t i;

    if (tab == NULL) {
        return NULL;
    }
    for (i = 0; i < tab->n; i++) {
        if (tab->t[i].used && eq32(tab->t[i].key, key)) {
            return &tab->t[i];
        }
    }
    return NULL;
}

int
wire_classify(const uint8_t *tx, uint32_t len, const wire_lut_tab_t *luts,
              wire_class_t *out)
{
    uint32_t off = 0, nsig = 0, nkeys = 0, nix = 0, nalt = 0, i;
    uint8_t enc = 0, keys[WIRE_KEY_MAX][32];
    uint32_t nvec = 0;
    uint8_t saw_pump = 0, saw_dlmm = 0, saw_router = 0, saw_watch = 0;
    uint8_t missing = 0, resolved = 0;

    if (tx == NULL || out == NULL || len < 8u) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (sv_get(tx, len, &off, &nsig) != 0 || nsig == 0 || nsig > 16u) {
        return -1;
    }
    if (off + nsig * 64u >= len) {
        return -1;
    }
    memcpy(out->sig, tx + off, 64);
    off += nsig * 64u;
    if ((tx[off] & 0x80u) != 0) {
        enc = (uint8_t)((tx[off] & 0x7fu) == 0 ? 1 : 2);
        off++;
    }
    out->encoding = enc;
    if (off + 3u > len) {
        return -1;
    }
    off += 3u;
    if (sv_get(tx, len, &off, &nkeys) != 0 || nkeys == 0) {
        return -1;
    }
    if (off + nkeys * 32u + 32u > len) {
        return -1;
    }
    nvec = nkeys;
    if (nvec > WIRE_KEY_MAX) {
        nvec = WIRE_KEY_MAX;
    }
    memcpy(keys, tx + off, nvec * 32u);
    off += nkeys * 32u + 32u;
    if (sv_get(tx, len, &off, &nix) != 0) {
        return -1;
    }
    out->n_ix = (uint16_t)nix;
    for (i = 0; i < nix; i++) {
        uint32_t prog, nac = 0, dlen = 0, a;

        if (off >= len) {
            return -1;
        }
        prog = tx[off++];
        if (sv_get(tx, len, &off, &nac) != 0) {
            return -1;
        }
        for (a = 0; a < nac; a++) {
            if (off >= len) {
                return -1;
            }
            off++;
        }
        if (sv_get(tx, len, &off, &dlen) != 0 || off + dlen > len) {
            return -1;
        }
        if (prog < nvec) {
            if (is_pump(keys[prog])) {
                saw_pump = 1;
            } else if (is_dlmm(keys[prog])) {
                saw_dlmm = 1;
            } else if (is_router(keys[prog])) {
                saw_router = 1;
            }
        }
        off += dlen;
    }
    if (enc != 0) {
        if (sv_get(tx, len, &off, &nalt) != 0) {
            return -1;
        }
        out->nalt = (uint8_t)(nalt > 255u ? 255u : nalt);
        for (i = 0; i < nalt; i++) {
            uint32_t nw = 0, nr = 0, j;
            const wire_lut_t *lt;

            if (off + 32u > len) {
                return -1;
            }
            lt = lut_find(luts, tx + off);
            off += 32u;
            if (sv_get(tx, len, &off, &nw) != 0
                || sv_get(tx, len, &off, &nr) != 0) {
                return -1;
            }
            for (j = 0; j < nw + nr; j++) {
                if (off >= len) {
                    return -1;
                }
                off++;
            }
            if (lt == NULL) {
                missing = 1;
            } else {
                uint32_t k;

                resolved = 1;
                for (k = 0; k < lt->n && nvec < WIRE_KEY_MAX; k++) {
                    memcpy(keys[nvec], lt->addr[k], 32);
                    nvec++;
                }
            }
        }
    }
    for (i = 0; i < nvec; i++) {
        if (is_watched(keys[i])) {
            saw_watch = 1;
        }
        if (is_pump(keys[i]) && !saw_pump && enc != 0) {
            saw_watch = 1;
        }
    }
    out->n_keys = (uint16_t)nvec;
    out->lut_resolved = resolved;
    out->lut_missing = missing;
    if (saw_pump) {
        out->klass = WIRE_CL_DIRECT_PUMP;
    } else if (saw_dlmm) {
        out->klass = WIRE_CL_DIRECT_DLMM;
    } else if (saw_router) {
        out->klass = WIRE_CL_KNOWN_ROUTER;
    } else if (saw_watch) {
        out->klass = WIRE_CL_WATCHED;
    } else if (missing) {
        out->klass = WIRE_CL_UNRESOLVED_ALT;
    } else {
        out->klass = WIRE_CL_OTHER;
    }
    return 0;
}

static uint8_t
fam_of(const uint8_t *k)
{
    if (eq32(k, PUMP_AMM)) {
        return WIRE_FAM_PUMP_AMM;
    }
    if (eq32(k, PUMP_BOND)) {
        return WIRE_FAM_PUMP_BOND;
    }
    if (eq32(k, DLMM)) {
        return WIRE_FAM_DLMM;
    }
    return WIRE_FAM_NONE;
}

int
wire_tx_ixs(const uint8_t *tx, uint32_t len, wire_ix_view_t *ix, uint32_t cap,
            uint32_t *n)
{
    uint32_t off = 0, nsig = 0, nkeys = 0, nix = 0, i, outn = 0;
    uint8_t keys[WIRE_KEY_MAX][32];
    uint32_t nvec;

    if (tx == NULL || n == NULL) {
        return -1;
    }
    *n = 0;
    if (sv_get(tx, len, &off, &nsig) != 0 || nsig == 0 || nsig > 16u) {
        return -1;
    }
    if (off + nsig * 64u >= len) {
        return -1;
    }
    off += nsig * 64u;
    if ((tx[off] & 0x80u) != 0) {
        off++;
    }
    if (off + 3u > len) {
        return -1;
    }
    off += 3u;
    if (sv_get(tx, len, &off, &nkeys) != 0 || nkeys == 0) {
        return -1;
    }
    if (off + nkeys * 32u + 32u > len) {
        return -1;
    }
    nvec = nkeys > WIRE_KEY_MAX ? WIRE_KEY_MAX : nkeys;
    memcpy(keys, tx + off, nvec * 32u);
    off += nkeys * 32u + 32u;
    if (sv_get(tx, len, &off, &nix) != 0) {
        return -1;
    }
    for (i = 0; i < nix; i++) {
        uint32_t prog, nac = 0, dlen = 0, a;
        wire_ix_view_t v;

        if (off >= len) {
            return -1;
        }
        prog = tx[off++];
        if (sv_get(tx, len, &off, &nac) != 0) {
            return -1;
        }
        for (a = 0; a < nac; a++) {
            if (off >= len) {
                return -1;
            }
            off++;
        }
        if (sv_get(tx, len, &off, &dlen) != 0 || off + dlen > len) {
            return -1;
        }
        memset(&v, 0, sizeof(v));
        v.prog_i = (uint8_t)prog;
        v.nacc = (uint8_t)(nac > 255u ? 255u : nac);
        v.dlen = dlen;
        if (dlen >= 8u) {
            memcpy(v.disc, tx + off, 8);
        } else if (dlen != 0) {
            memcpy(v.disc, tx + off, dlen);
        }
        if (prog < nvec) {
            memcpy(v.prog, keys[prog], 32);
            v.fam = fam_of(v.prog);
        }
        if (ix != NULL && outn < cap) {
            ix[outn] = v;
        }
        outn++;
        off += dlen;
    }
    *n = outn;
    return 0;
}
