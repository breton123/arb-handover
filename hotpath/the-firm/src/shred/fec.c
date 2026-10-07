#include "shred/fec.h"

#include "net/time.h"
#include "shred/merkle.h"
#include "shred/rs.h"

#include <stdlib.h>
#include <string.h>

#define FEC_SET_CAP  1024u
#define FEC_SHARD_N  (SHRED_RS_MAX * 2u)

typedef struct {
    uint8_t  have;
    uint16_t len;
    uint8_t  pkt[SHRED_MAX_SZ];
} fec_shard_t;

typedef struct {
    uint8_t  used;
    uint8_t  frozen;
    uint8_t  done;
    uint8_t  possible;
    uint8_t  chained;
    uint8_t  resigned;
    uint8_t  proof_n;
    uint16_t n;
    uint16_t k;
    uint32_t fec;
    uint64_t slot;
    uint64_t tsc_first;
    uint8_t  sig[64];
    uint8_t  have_sig;
    uint8_t  root[32];
    uint8_t  have_root;
    uint8_t  chained_root[32];
    uint8_t  have_chained;
    uint16_t n_have;
    fec_shard_t sh[FEC_SHARD_N];
} fec_set_t;

struct shred_fec {
    fec_set_t set[FEC_SET_CAP];
    uint32_t  victim;
    shred_fec_stats_t st;
};

static uint32_t
mix(uint64_t slot, uint32_t fec)
{
    uint64_t x = slot ^ ((uint64_t)fec * 0x9e3779b97f4a7c15ULL);
    x ^= x >> 32;
    return (uint32_t)x;
}

static int
is_resigned(uint8_t type)
{
    uint8_t h = (uint8_t)(type & 0xF0u);
    return h == SHRED_TYPE_RESIGNED_CODE || h == SHRED_TYPE_RESIGNED_DATA;
}

static int
is_chained(uint8_t type)
{
    uint8_t h = (uint8_t)(type & 0xF0u);
    return h == SHRED_TYPE_CHAINED_CODE || h == SHRED_TYPE_CHAINED_DATA
        || is_resigned(type);
}

static uint32_t
data_cap(uint8_t proof_n, int chained, int resigned)
{
    uint32_t v = 1203u - SHRED_DATA_HDR_SZ - (uint32_t)proof_n * 20u;
    if (chained) {
        v -= 32u;
    }
    if (resigned) {
        v -= 64u;
    }
    return v;
}

static uint32_t
code_cap(uint8_t proof_n, int chained, int resigned)
{
    uint32_t v = 1228u - SHRED_CODE_HDR_SZ - (uint32_t)proof_n * 20u;
    if (chained) {
        v -= 32u;
    }
    if (resigned) {
        v -= 64u;
    }
    return v;
}

int
shred_fec_init(shred_fec_t **out)
{
    shred_fec_t *f;

    if (out == NULL) {
        return -1;
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL) {
        return -1;
    }
    *out = f;
    return 0;
}

void
shred_fec_free(shred_fec_t *f)
{
    free(f);
}

const shred_fec_stats_t *
shred_fec_stats(const shred_fec_t *f)
{
    return f != NULL ? &f->st : NULL;
}

static fec_set_t *
set_of(shred_fec_t *f, uint64_t slot, uint32_t fec, int create)
{
    uint32_t i, h = mix(slot, fec) & (FEC_SET_CAP - 1u);

    for (i = 0; i < 16; i++) {
        fec_set_t *s = &f->set[(h + i) & (FEC_SET_CAP - 1u)];
        if (s->used && s->slot == slot && s->fec == fec) {
            return s;
        }
        if (!s->used) {
            if (!create) {
                return NULL;
            }
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->slot = slot;
            s->fec = fec;
            return s;
        }
    }
    if (!create) {
        return NULL;
    }
    {
        fec_set_t *s = &f->set[f->victim++ & (FEC_SET_CAP - 1u)];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->slot = slot;
        s->fec = fec;
        return s;
    }
}

static int
layout(const uint8_t *pkt, uint16_t len, shred_view_t *v, uint32_t *ei,
       uint16_t *n, uint16_t *k, uint32_t *eoff, uint32_t *elen,
       uint32_t *poff)
{
    uint8_t pn, chained, resigned;
    uint32_t dcap, ccap;

    if (shred_parse(pkt, len, v) != 0) {
        return -1;
    }
    pn = (uint8_t)(v->type & 0x0fu);
    if (pn == 0 || pn > 8) {
        return -1;
    }
    chained = (uint8_t)is_chained(v->type);
    resigned = (uint8_t)is_resigned(v->type);
    dcap = data_cap(pn, chained, resigned);
    ccap = code_cap(pn, chained, resigned);
    if (shred_is_data(v->type)) {
        if (v->index < v->fec_set) {
            return -1;
        }
        *ei = v->index - v->fec_set;
        *eoff = 64;
        *elen = 24u + dcap;
        *poff = SHRED_DATA_HDR_SZ + dcap + (chained ? 32u : 0u);
        *n = 0;
        *k = 0;
    } else if (shred_is_code(v->type)) {
        uint16_t nd, nk, pos;
        if (len < SHRED_CODE_HDR_SZ) {
            return -1;
        }
        nd = shred_load_u16_le(pkt + 83);
        nk = shred_load_u16_le(pkt + 85);
        pos = shred_load_u16_le(pkt + 87);
        if (nd == 0 || nd > SHRED_RS_MAX || nk == 0 || nk > SHRED_RS_MAX
            || pos >= nk) {
            return -1;
        }
        *n = nd;
        *k = nk;
        *ei = (uint32_t)nd + pos;
        *eoff = SHRED_CODE_HDR_SZ;
        *elen = ccap;
        *poff = SHRED_CODE_HDR_SZ + ccap + (chained ? 32u : 0u);
    } else {
        return -1;
    }
    if (*poff + (uint32_t)pn * 20u > len) {
        return -1;
    }
    (void)dcap;
    return 0;
}

static int
root_of(const uint8_t *pkt, uint16_t len, uint32_t ei, uint8_t out[32])
{
    shred_view_t v;
    uint16_t n, k;
    uint32_t eoff, elen, poff;
    uint8_t leaf[32];

    if (layout(pkt, len, &v, &ei, &n, &k, &eoff, &elen, &poff) != 0) {
        return -1;
    }
    (void)eoff;
    (void)elen;
    shred_merkle_leaf(pkt + 64, poff - 64u, leaf);
    return shred_merkle_root_from_proof(ei, leaf, pkt + poff,
                                        (uint32_t)(v.type & 0x0fu), out);
}

int
shred_fec_merkle_root(const uint8_t *pkt, uint16_t len, uint8_t root[32])
{
    uint32_t ei = 0;

    if (root == NULL) {
        return -1;
    }
    return root_of(pkt, len, ei, root);
}

static int
try_recover(shred_fec_t *f, fec_set_t *s, uint64_t tsc_enough,
            shred_fec_rec_t *rec, uint32_t cap, uint32_t *nrec)
{
    uint8_t *ptr[FEC_SHARD_N];
    uint8_t have[FEC_SHARD_N];
    uint8_t miss[SHRED_RS_MAX][1200];
    uint8_t code_miss[SHRED_RS_MAX][1200];
    uint8_t leaves[134][32];
    uint8_t root[32];
    uint32_t i, total, elen, dcap, ccap;
    uint64_t ns0, ns1;

    if (s->n == 0 || s->k == 0 || s->done || s->frozen || !s->have_root) {
        return 0;
    }
    total = (uint32_t)s->n + (uint32_t)s->k;
    if (s->n_have < s->n || total > FEC_SHARD_N) {
        return 0;
    }
    if (!s->possible) {
        s->possible = 1;
        f->st.possible++;
    }
    {
        uint32_t need = 0;
        for (i = 0; i < s->n; i++) {
            if (!s->sh[i].have) {
                need = 1;
                break;
            }
        }
        if (!need) {
            return 0;
        }
    }
    dcap = data_cap(s->proof_n, s->chained, s->resigned);
    ccap = code_cap(s->proof_n, s->chained, s->resigned);
    elen = 24u + dcap;
    if (elen > 1200 || ccap > 1200 || elen != ccap) {
        s->frozen = 1;
        f->st.auth_fail++;
        return 0;
    }
    memset(ptr, 0, sizeof(ptr));
    memset(have, 0, sizeof(have));
    for (i = 0; i < total; i++) {
        if (s->sh[i].have) {
            uint32_t eoff = (i < s->n) ? 64u : SHRED_CODE_HDR_SZ;
            uint32_t sl = elen;
            if ((uint32_t)s->sh[i].len < eoff + sl) {
                s->frozen = 1;
                f->st.auth_fail++;
                return 0;
            }
            ptr[i] = s->sh[i].pkt + eoff;
            have[i] = 1;
        } else if (i < s->n) {
            ptr[i] = miss[i];
        } else {
            ptr[i] = code_miss[i - s->n];
        }
    }
    f->st.attempted++;
    ns0 = net_now_ns();
    if (shred_rs_recover(ptr, have, s->n, s->k, elen) != 0) {
        ns1 = net_now_ns();
        f->st.recovery_cpu_ns += ns1 - ns0;
        s->frozen = 1;
        f->st.auth_fail++;
        return 0;
    }
    ns1 = net_now_ns();
    f->st.recovery_cpu_ns += ns1 - ns0;

    ns0 = net_now_ns();
    for (i = 0; i < total; i++) {
        uint32_t poff;
        uint8_t tmp[SHRED_MAX_SZ];

        if (i < s->n) {
            poff = SHRED_DATA_HDR_SZ + dcap + (s->chained ? 32u : 0u);
        } else {
            poff = SHRED_CODE_HDR_SZ + ccap + (s->chained ? 32u : 0u);
        }
        if (s->sh[i].have) {
            if ((uint32_t)s->sh[i].len < poff) {
                f->st.auth_fail++;
                s->frozen = 1;
                return 0;
            }
            shred_merkle_leaf(s->sh[i].pkt + 64, poff - 64u, leaves[i]);
            continue;
        }
        memset(tmp, 0, sizeof(tmp));
        memcpy(tmp, s->sig, 64);
        if (i < s->n) {
            memcpy(tmp + 64, miss[i], elen);
            if (s->chained && s->have_chained) {
                memcpy(tmp + SHRED_DATA_HDR_SZ + dcap, s->chained_root, 32);
            }
            shred_merkle_leaf(tmp + 64, poff - 64u, leaves[i]);
        } else {
            uint16_t pos = (uint16_t)(i - s->n);
            uint32_t idx = s->fec + pos;
            uint16_t ver = 0;
            uint32_t p;
            tmp[64] = (uint8_t)((s->resigned ? SHRED_TYPE_RESIGNED_CODE
                                             : SHRED_TYPE_CHAINED_CODE)
                                | s->proof_n);
            memcpy(tmp + 65, &s->slot, 8);
            memcpy(tmp + 73, &idx, 4);
            for (p = 0; p < total; p++) {
                if (s->sh[p].have && s->sh[p].len >= 79) {
                    memcpy(&ver, s->sh[p].pkt + 77, 2);
                    break;
                }
            }
            memcpy(tmp + 77, &ver, 2);
            memcpy(tmp + 79, &s->fec, 4);
            memcpy(tmp + 83, &s->n, 2);
            memcpy(tmp + 85, &s->k, 2);
            memcpy(tmp + 87, &pos, 2);
            memcpy(tmp + 89, code_miss[i - s->n], ccap);
            if (s->chained && s->have_chained) {
                memcpy(tmp + 89 + ccap, s->chained_root, 32);
            }
            shred_merkle_leaf(tmp + 64, poff - 64u, leaves[i]);
        }
    }
    if (shred_merkle_root(&leaves[0][0], total, root) != 0
        || memcmp(root, s->root, 32) != 0) {
        ns1 = net_now_ns();
        f->st.auth_cpu_ns += ns1 - ns0;
        f->st.auth_fail++;
        s->frozen = 1;
        return 0;
    }
    ns1 = net_now_ns();
    f->st.auth_cpu_ns += ns1 - ns0;
    f->st.auth_ok++;
    f->st.recovered++;
    s->done = 1;
    *nrec = 0;
    for (i = 0; i < s->n && *nrec < cap; i++) {
        shred_fec_rec_t *o;
        if (s->sh[i].have) {
            continue;
        }
        o = &rec[*nrec];
        memset(o, 0, sizeof(*o));
        memcpy(o->pkt, s->sig, 64);
        memcpy(o->pkt + 64, miss[i], elen);
        if (s->chained && s->have_chained) {
            memcpy(o->pkt + SHRED_DATA_HDR_SZ + dcap, s->chained_root, 32);
        }
        o->len = 1203;
        o->slot = s->slot;
        o->fec = s->fec;
        o->tsc_first = s->tsc_first;
        o->tsc_enough = tsc_enough;
        o->index = shred_load_u32_le(o->pkt + SHRED_OFF_INDEX);
        (*nrec)++;
    }
    return (*nrec != 0) ? 1 : 0;
}

int
shred_fec_push(shred_fec_t *f, const uint8_t *pkt, uint16_t len,
               uint64_t rx_tsc, shred_fec_rec_t *rec, uint32_t cap,
               uint32_t *nrec)
{
    shred_view_t v;
    fec_set_t *s;
    uint32_t ei, eoff, elen, poff;
    uint16_t n, k;
    uint8_t root[32];
    uint64_t ns0, ns1;
    int rc;

    if (f == NULL || pkt == NULL || nrec == NULL) {
        return -1;
    }
    *nrec = 0;
    ns0 = net_now_ns();
    if (layout(pkt, len, &v, &ei, &n, &k, &eoff, &elen, &poff) != 0) {
        ns1 = net_now_ns();
        f->st.parse_cpu_ns += ns1 - ns0;
        return -1;
    }
    ns1 = net_now_ns();
    f->st.parse_cpu_ns += ns1 - ns0;
    if (ei >= FEC_SHARD_N) {
        return -1;
    }
    s = set_of(f, v.slot, v.fec_set, 1);
    if (s == NULL || s->frozen) {
        return -1;
    }
    if (s->tsc_first == 0 || rx_tsc < s->tsc_first) {
        s->tsc_first = rx_tsc;
    }
    s->proof_n = (uint8_t)(v.type & 0x0fu);
    s->chained = (uint8_t)is_chained(v.type);
    s->resigned = (uint8_t)is_resigned(v.type);
    if (n != 0) {
        if (s->n != 0 && (s->n != n || s->k != k)) {
            s->frozen = 1;
            f->st.auth_fail++;
            return -1;
        }
        s->n = n;
        s->k = k;
    }
    if (!s->have_sig) {
        memcpy(s->sig, pkt, 64);
        s->have_sig = 1;
    } else if (memcmp(s->sig, pkt, 64) != 0) {
        s->frozen = 1;
        f->st.auth_fail++;
        return -1;
    }
    if (root_of(pkt, len, ei, root) != 0) {
        s->frozen = 1;
        f->st.auth_fail++;
        return -1;
    }
    if (!s->have_root) {
        memcpy(s->root, root, 32);
        s->have_root = 1;
    } else if (memcmp(s->root, root, 32) != 0) {
        s->frozen = 1;
        f->st.auth_fail++;
        return -1;
    }
    if (s->chained && !s->have_chained) {
        uint32_t croff = poff - 32u;
        if (len >= croff + 32u) {
            memcpy(s->chained_root, pkt + croff, 32);
            s->have_chained = 1;
        }
    }
    if (s->sh[ei].have) {
        uint32_t sl = elen;
        if (s->sh[ei].len != len
            || memcmp(s->sh[ei].pkt + eoff, pkt + eoff, sl) != 0) {
            s->frozen = 1;
            f->st.auth_fail++;
            return -1;
        }
        return 0;
    }
    if (len > SHRED_MAX_SZ) {
        return -1;
    }
    s->sh[ei].have = 1;
    s->sh[ei].len = len;
    memcpy(s->sh[ei].pkt, pkt, len);
    s->n_have++;
    rc = try_recover(f, s, rx_tsc, rec, cap, nrec);
    return rc;
}
