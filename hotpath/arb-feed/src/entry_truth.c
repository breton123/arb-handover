/*
 * ENTRY-TRUTH — read-only. Reconstruct CAP-002 Entry batches and ask
 * whether each opp_synced N is a deserialized Entry transaction or
 * only a shred-payload fragment. Does not write .cap. Does not send.
 */
#include "feed.h"
#include "shred.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SHRED_OFF_FLAGS  85u
#define SHRED_OFF_SIZE   86u
#define DATA_HDR         88u
#define DATA_COMPLETE    0x80u
#define OPEN_SLOTS       256u
#define TGT_MAX          512u
#define TX_SIG_CAP       4096u
#define HT               2048u

typedef struct {
    uint32_t index;
    uint32_t fec;
    uint32_t arena_off;
    uint16_t plen;
    uint8_t  flags;
    uint64_t rx_ns;
} drec_t;

typedef struct {
    uint64_t slot;
    uint8_t  used;
    drec_t  *d;
    uint32_t dn, dcap;
    uint8_t *arena;
    uint32_t arena_n, arena_cap;
} slot_t;

typedef struct {
    uint8_t  sig[64];
    char     hex[129];
    int      shred_hit;
    uint64_t shred_slot;
    uint32_t shred_index;
    uint32_t shred_fec;
    uint8_t  shred_type;
    uint64_t shred_rx_ns;
    uint32_t payload_off;
    int      batch_seen;
    int      batch_ok;
    int      batch_gap;
    int      entry_hit;
    uint32_t entry_i;
    uint32_t tx_i;
    uint64_t complete_rx_ns;
    uint32_t batch_i0;
    uint32_t batch_i1;
} tgt_t;

static tgt_t g_tgt[TGT_MAX];
static uint32_t g_ntgt, g_unhit;
static int16_t g_ht[HT];
static uint64_t g_shred_data, g_batches, g_batches_ok, g_batches_gap, g_parse_fail;

static uint32_t
h8(uint64_t k)
{
    return ((uint32_t)k ^ (uint32_t)(k >> 32)) & (HT - 1u);
}

static void
ht_build(void)
{
    uint32_t i;
    memset(g_ht, 0xff, sizeof(g_ht));
    for (i = 0; i < g_ntgt; i++) {
        uint64_t k;
        uint32_t h;
        memcpy(&k, g_tgt[i].sig, 8);
        h = h8(k);
        while (g_ht[h] >= 0) {
            h = (h + 1u) & (HT - 1u);
        }
        g_ht[h] = (int16_t)i;
    }
    g_unhit = g_ntgt;
}

static int
hex_nib(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int
load_sigs(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (f == NULL) {
        perror(path);
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL && g_ntgt < TGT_MAX) {
        size_t n = strcspn(line, "\r\n");
        uint32_t i;
        tgt_t *t;

        if (n != 128) {
            continue;
        }
        t = &g_tgt[g_ntgt];
        memset(t, 0, sizeof(*t));
        for (i = 0; i < 64; i++) {
            int hi = hex_nib(line[i * 2u]);
            int lo = hex_nib(line[i * 2u + 1u]);
            if (hi < 0 || lo < 0) {
                t = NULL;
                break;
            }
            t->sig[i] = (uint8_t)((hi << 4) | lo);
        }
        if (t == NULL) {
            continue;
        }
        memcpy(t->hex, line, 128);
        t->hex[128] = 0;
        g_ntgt++;
    }
    fclose(f);
    return 0;
}

static int
cu32(const uint8_t *p, uint32_t avail, uint32_t *val, uint32_t *used)
{
    if (avail < 1) {
        return -1;
    }
    if ((p[0] & 0x80u) == 0) {
        *val = p[0];
        *used = 1;
        return 0;
    }
    if (avail < 2 || (p[1] & 0x80u) != 0) {
        if (avail < 3 || (p[2] & 0xfcu) != 0 || p[2] == 0) {
            return -1;
        }
        *val = (uint32_t)(p[0] & 0x7fu)
             | ((uint32_t)(p[1] & 0x7fu) << 7)
             | ((uint32_t)p[2] << 14);
        *used = 3;
        return 0;
    }
    if (p[1] == 0) {
        return -1;
    }
    *val = (uint32_t)(p[0] & 0x7fu) | ((uint32_t)p[1] << 7);
    *used = 2;
    return 0;
}

static int
skip_ix(const uint8_t *p, uint32_t len, uint32_t *off)
{
    uint32_t used, nacc, dlen;

    if (*off + 1u > len) {
        return -1;
    }
    *off += 1u;
    if (cu32(p + *off, len - *off, &nacc, &used) != 0) {
        return -1;
    }
    *off += used + nacc;
    if (*off > len) {
        return -1;
    }
    if (cu32(p + *off, len - *off, &dlen, &used) != 0) {
        return -1;
    }
    *off += used + dlen;
    return (*off > len) ? -1 : 0;
}

static int
skip_tx(const uint8_t *p, uint32_t len, uint32_t *off, uint8_t first[64])
{
    uint32_t used, nsig, nkeys, ninstr, i, nlut;
    uint8_t b0, versioned = 0;

    if (cu32(p + *off, len - *off, &nsig, &used) != 0 || nsig == 0 || nsig > 64) {
        return -1;
    }
    *off += used;
    if (*off + nsig * 64u > len) {
        return -1;
    }
    memcpy(first, p + *off, 64);
    *off += nsig * 64u;
    if (*off >= len) {
        return -1;
    }
    b0 = p[*off];
    *off += 1;
    if ((b0 & 0x80u) != 0) {
        if ((b0 & 0x7fu) != 0) {
            return -1;
        }
        versioned = 1;
        if (*off >= len || p[*off] != (uint8_t)nsig) {
            return -1;
        }
        *off += 1;
    } else if (b0 != (uint8_t)nsig) {
        return -1;
    }
    if (*off + 2u > len) {
        return -1;
    }
    *off += 2;
    if (cu32(p + *off, len - *off, &nkeys, &used) != 0 || nkeys == 0 || nkeys > 256) {
        return -1;
    }
    *off += used + nkeys * 32u + 32u;
    if (*off > len) {
        return -1;
    }
    if (cu32(p + *off, len - *off, &ninstr, &used) != 0 || ninstr > 256) {
        return -1;
    }
    *off += used;
    for (i = 0; i < ninstr; i++) {
        if (skip_ix(p, len, off) != 0) {
            return -1;
        }
    }
    if (versioned) {
        if (cu32(p + *off, len - *off, &nlut, &used) != 0 || nlut > 32) {
            return -1;
        }
        *off += used;
        for (i = 0; i < nlut; i++) {
            uint32_t nw, nr;
            if (*off + 32u > len) {
                return -1;
            }
            *off += 32u;
            if (cu32(p + *off, len - *off, &nw, &used) != 0) {
                return -1;
            }
            *off += used + nw;
            if (cu32(p + *off, len - *off, &nr, &used) != 0) {
                return -1;
            }
            *off += used + nr;
            if (*off > len) {
                return -1;
            }
        }
    }
    return 0;
}

static int
framed_sig(const uint8_t *p, uint32_t len, uint32_t pos)
{
    uint32_t back;
    uint8_t first[64];

    if (pos >= len || pos + 64u > len) {
        return 0;
    }
    for (back = 1; back <= 3 && back <= pos; back++) {
        uint32_t off = pos - back;
        uint32_t save = off;
        if (skip_tx(p, len, &off, first) == 0
            && memcmp(first, p + pos, 64) == 0) {
            (void)save;
            return 1;
        }
    }
    return 0;
}

static int
collect_sigs(const uint8_t *p, uint32_t len, uint8_t sigs[][64],
             uint32_t *n, uint32_t cap)
{
    uint64_t nent, e;
    uint32_t off = 8;

    *n = 0;
    if (len < 8) {
        return -1;
    }
    memcpy(&nent, p, 8);
    if (nent == 0 || nent > 4096) {
        return -1;
    }
    for (e = 0; e < nent; e++) {
        uint64_t ntx, i;
        if (off + 48u > len) {
            return (*n > 0) ? 0 : -1;
        }
        off += 40u;
        memcpy(&ntx, p + off, 8);
        off += 8;
        if (ntx > 4096) {
            return (*n > 0) ? 0 : -1;
        }
        for (i = 0; i < ntx; i++) {
            uint8_t first[64];
            if (skip_tx(p, len, &off, first) != 0) {
                return (*n > 0) ? 0 : -1;
            }
            if (*n < cap) {
                memcpy(sigs[*n], first, 64);
                (*n)++;
            }
        }
    }
    return 0;
}

static int
cmp_drec(const void *a, const void *b)
{
    const drec_t *x = a, *y = b;
    if (x->index < y->index) {
        return -1;
    }
    if (x->index > y->index) {
        return 1;
    }
    return 0;
}

static void
note_shred(const uint8_t *pay, uint16_t payn, const shred_view_t *v,
           uint64_t rx)
{
    uint32_t off;

    if (pay == NULL || payn < 64 || g_unhit == 0) {
        return;
    }
    for (off = 0; off + 64u <= payn; off++) {
        uint64_t k;
        uint32_t h;
        memcpy(&k, pay + off, 8);
        h = h8(k);
        while (g_ht[h] >= 0) {
            uint32_t idx = (uint32_t)g_ht[h];
            uint64_t tk;
            memcpy(&tk, g_tgt[idx].sig, 8);
            if (tk == k && !g_tgt[idx].shred_hit
                && memcmp(pay + off, g_tgt[idx].sig, 64) == 0) {
                g_tgt[idx].shred_hit = 1;
                g_tgt[idx].shred_slot = v->slot;
                g_tgt[idx].shred_index = v->index;
                g_tgt[idx].shred_fec = v->fec_set;
                g_tgt[idx].shred_type = v->type;
                g_tgt[idx].shred_rx_ns = rx;
                g_tgt[idx].payload_off = off;
                g_unhit--;
            }
            h = (h + 1u) & (HT - 1u);
        }
    }
}

static void
apply_batch(slot_t *s, uint32_t run0, uint32_t end, int ok_run, int parse_ok,
            uint8_t sigs[][64], uint32_t nsig, const uint8_t *buf, uint32_t blen)
{
    uint32_t i0 = s->d[run0].index;
    uint32_t i1 = s->d[end].index;
    uint64_t slot = s->slot;
    uint64_t done = s->d[end].rx_ns;
    uint32_t t, k;

    for (t = 0; t < g_ntgt; t++) {
        tgt_t *g = &g_tgt[t];
        if (!g->shred_hit || g->shred_slot != slot) {
            continue;
        }
        if (g->shred_index < i0 || g->shred_index > i1) {
            continue;
        }
        g->batch_seen = 1;
        g->complete_rx_ns = done;
        g->batch_i0 = i0;
        g->batch_i1 = i1;
        if (!ok_run) {
            g->batch_gap = 1;
            continue;
        }
        if (parse_ok) {
            g->batch_ok = 1;
        }
        for (k = 0; k < nsig; k++) {
            if (memcmp(sigs[k], g->sig, 64) == 0) {
                g->entry_hit = 1;
                g->tx_i = k;
                g->batch_ok = 1;
                break;
            }
        }
        if (!g->entry_hit && buf != NULL && blen >= 64) {
            uint32_t pos;
            for (pos = 0; pos + 64u <= blen; pos++) {
                if (memcmp(buf + pos, g->sig, 64) == 0
                    && framed_sig(buf, blen, pos)) {
                    g->entry_hit = 1;
                    g->batch_ok = 1;
                    break;
                }
            }
        }
    }
}

static void
flush_slot(slot_t *s)
{
    uint32_t i, run0, expect;

    if (!s->used || s->dn == 0) {
        goto done;
    }
    qsort(s->d, s->dn, sizeof(drec_t), cmp_drec);
    run0 = 0;
    expect = s->d[0].index;
    for (i = 0; i < s->dn; i++) {
        int complete = (s->d[i].flags & DATA_COMPLETE) != 0;
        if (s->d[i].index != expect) {
            if (complete) {
                g_batches++;
                g_batches_gap++;
                apply_batch(s, run0, i, 0, 0, NULL, 0, NULL, 0);
            }
            run0 = i;
            expect = s->d[i].index;
        }
        if (complete) {
            uint32_t j, blen = 0;
            uint8_t *buf;
            int ok = 1;
            uint8_t sigs[TX_SIG_CAP][64];
            uint32_t nsig = 0;
            int pok = 0;

            g_batches++;
            for (j = run0; j <= i; j++) {
                if (s->d[j].index != s->d[run0].index + (j - run0)) {
                    ok = 0;
                    break;
                }
                blen += s->d[j].plen;
            }
            if (!ok) {
                g_batches_gap++;
                apply_batch(s, run0, i, 0, 0, NULL, 0, NULL, 0);
            } else {
                buf = malloc(blen == 0 ? 1 : blen);
                if (buf == NULL) {
                    goto done;
                }
                blen = 0;
                for (j = run0; j <= i; j++) {
                    memcpy(buf + blen, s->arena + s->d[j].arena_off, s->d[j].plen);
                    blen += s->d[j].plen;
                }
                if (collect_sigs(buf, blen, sigs, &nsig, TX_SIG_CAP) == 0) {
                    g_batches_ok++;
                    pok = 1;
                } else {
                    g_parse_fail++;
                }
                apply_batch(s, run0, i, 1, pok, sigs, nsig, buf, blen);
                free(buf);
            }
            if (i + 1u < s->dn) {
                run0 = i + 1u;
                expect = s->d[i + 1u].index;
            } else {
                expect = s->d[i].index + 1u;
                run0 = i + 1u;
            }
            continue;
        }
        expect++;
    }
done:
    free(s->d);
    free(s->arena);
    memset(s, 0, sizeof(*s));
}

static slot_t *
slot_get(slot_t *tab, uint64_t slot)
{
    uint32_t i, empty = OPEN_SLOTS, oldest = 0;
    uint64_t oldest_slot = UINT64_MAX;

    for (i = 0; i < OPEN_SLOTS; i++) {
        if (tab[i].used && tab[i].slot == slot) {
            return &tab[i];
        }
        if (!tab[i].used && empty == OPEN_SLOTS) {
            empty = i;
        }
        if (tab[i].used && tab[i].slot < oldest_slot) {
            oldest_slot = tab[i].slot;
            oldest = i;
        }
    }
    if (empty == OPEN_SLOTS) {
        flush_slot(&tab[oldest]);
        empty = oldest;
    }
    tab[empty].used = 1;
    tab[empty].slot = slot;
    return &tab[empty];
}

static int
add_data(slot_t *s, const shred_view_t *v, const uint8_t *pkt, uint16_t plen,
         uint64_t rx)
{
    uint16_t size, pay;
    drec_t *nd;

    if (plen < DATA_HDR + 1u) {
        return 0;
    }
    size = shred_load_u16_le(pkt + SHRED_OFF_SIZE);
    if (size < DATA_HDR || size > plen) {
        return 0;
    }
    pay = (uint16_t)(size - DATA_HDR);
    if (s->dn == s->dcap) {
        uint32_t cap = s->dcap == 0 ? 256 : s->dcap * 2;
        nd = realloc(s->d, cap * sizeof(*nd));
        if (nd == NULL) {
            return -1;
        }
        s->d = nd;
        s->dcap = cap;
    }
    if (s->arena_n + pay > s->arena_cap) {
        uint32_t cap = s->arena_cap == 0 ? (1u << 16) : s->arena_cap;
        uint8_t *na;
        while (cap < s->arena_n + pay) {
            cap *= 2u;
        }
        na = realloc(s->arena, cap);
        if (na == NULL) {
            return -1;
        }
        s->arena = na;
        s->arena_cap = cap;
    }
    memcpy(s->arena + s->arena_n, pkt + DATA_HDR, pay);
    note_shred(pkt + DATA_HDR, pay, v, rx);
    nd = &s->d[s->dn++];
    nd->index = v->index;
    nd->fec = v->fec_set;
    nd->arena_off = s->arena_n;
    nd->plen = pay;
    nd->flags = pkt[SHRED_OFF_FLAGS];
    nd->rx_ns = rx;
    s->arena_n += pay;
    return 0;
}

static int
walk_file(slot_t *tab, const char *path)
{
    FILE *f;
    feedcap_hdr_t hdr;
    feed_slot_t slot;

    f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (feedcap_read_header(f, &hdr) != 0) {
        fprintf(stderr, "bad header %s\n", path);
        fclose(f);
        return 0;
    }
    fprintf(stderr, "read  %s\n", path);
    for (;;) {
        shred_view_t v;
        slot_t *s;
        int rc = feedcap_read_slot(f, &slot);
        if (rc != 0) {
            break;
        }
        if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
            continue;
        }
        if (!shred_is_data(v.type)) {
            continue;
        }
        g_shred_data++;
        s = slot_get(tab, v.slot);
        if (add_data(s, &v, slot.data, (uint16_t)slot.len, slot.rx_ns) != 0) {
            fprintf(stderr, "add_data fail %s slot=%" PRIu64 "\n", path, v.slot);
            break;
        }
    }
    fclose(f);
    return 0;
}

static int
cmp_mtime(const void *a, const void *b)
{
    const char *const *pa = a, *const *pb = b;
    struct stat sa, sb;
    time_t ta = 0, tb = 0;
    if (stat(*pa, &sa) == 0) {
        ta = sa.st_mtime;
    }
    if (stat(*pb, &sb) == 0) {
        tb = sb.st_mtime;
    }
    if (ta < tb) {
        return -1;
    }
    if (ta > tb) {
        return 1;
    }
    return strcmp(*pa, *pb);
}

static int
list_caps(const char *dir, const char *prefix, uint32_t newest,
          char ***out, uint32_t *n)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    char **paths = NULL;
    uint32_t cap = 0, i;

    *out = NULL;
    *n = 0;
    if (d == NULL) {
        perror(dir);
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t L = strlen(ent->d_name);
        char *p;
        if (L < 5 || strcmp(ent->d_name + L - 4, ".cap") != 0) {
            continue;
        }
        if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        if (*n == cap) {
            uint32_t nc = cap == 0 ? 16 : cap * 2;
            char **np = realloc(paths, nc * sizeof(*np));
            if (np == NULL) {
                closedir(d);
                return -1;
            }
            paths = np;
            cap = nc;
        }
        p = malloc(strlen(dir) + 1 + L + 1);
        if (p == NULL) {
            closedir(d);
            return -1;
        }
        sprintf(p, "%s/%s", dir, ent->d_name);
        paths[(*n)++] = p;
    }
    closedir(d);
    qsort(paths, *n, sizeof(*paths), cmp_mtime);
    if (newest > 0 && *n > newest) {
        uint32_t drop = *n - newest;
        for (i = 0; i < drop; i++) {
            free(paths[i]);
        }
        memmove(paths, paths + drop, newest * sizeof(*paths));
        *n = newest;
    }
    *out = paths;
    return 0;
}

static const char *
klass(const tgt_t *t)
{
    if (!t->shred_hit) {
        return "NO_CAP";
    }
    if (t->entry_hit) {
        return "ENTRY_VALID";
    }
    if (t->batch_ok) {
        return "FALSE_TRIGGER";
    }
    return "FRAGMENT_ONLY";
}

int
main(int argc, char **argv)
{
    slot_t tab[OPEN_SLOTS];
    const char *dir = NULL, *sigs = NULL, *prefix = "orbitflare-";
    uint32_t newest = 20, i;
    char **paths = NULL;
    uint32_t npath = 0;

    memset(tab, 0, sizeof(tab));
    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < (uint32_t)argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--sigs") == 0 && i + 1 < (uint32_t)argc) {
            sigs = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < (uint32_t)argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--newest") == 0 && i + 1 < (uint32_t)argc) {
            newest = (uint32_t)atoi(argv[++i]);
        }
    }
    if (dir == NULL || sigs == NULL) {
        fprintf(stderr, "usage: %s --dir DIR --sigs HEX [--newest N]\n", argv[0]);
        return 1;
    }
    if (load_sigs(sigs) != 0 || g_ntgt == 0) {
        fprintf(stderr, "no sigs\n");
        return 1;
    }
    ht_build();
    fprintf(stderr, "ENTRY-TRUTH  targets=%u  dir=%s  newest=%u  NO WRITE\n",
            g_ntgt, dir, newest);
    if (list_caps(dir, prefix, newest, &paths, &npath) != 0) {
        return 1;
    }
    for (i = 0; i < npath; i++) {
        if (walk_file(tab, paths[i]) != 0) {
            fprintf(stderr, "skip %s\n", paths[i]);
        }
    }
    for (i = 0; i < OPEN_SLOTS; i++) {
        flush_slot(&tab[i]);
    }
    for (i = 0; i < g_ntgt; i++) {
        const tgt_t *t = &g_tgt[i];
        int64_t dt = 0;
        if (t->shred_hit && t->complete_rx_ns >= t->shred_rx_ns) {
            dt = (int64_t)(t->complete_rx_ns - t->shred_rx_ns);
        }
        printf("{\"sig_hex\":\"%s\",\"class\":\"%s\",\"shred_hit\":%d"
               ",\"shred\":{\"slot\":%" PRIu64 ",\"index\":%u,\"fec\":%u,"
               "\"type\":%u,\"rx_ns\":%" PRIu64 ",\"payload_off\":%u}"
               ",\"batch_seen\":%d,\"batch_ok\":%d,\"batch_gap\":%d"
               ",\"entry_hit\":%d,\"tx_i\":%u,\"batch_i0\":%u,\"batch_i1\":%u"
               ",\"complete_rx_ns\":%" PRIu64 ",\"dt_ns\":%" PRId64 "}\n",
               t->hex, klass(t), t->shred_hit,
               t->shred_slot, t->shred_index, t->shred_fec,
               (unsigned)t->shred_type, t->shred_rx_ns, t->payload_off,
               t->batch_seen, t->batch_ok, t->batch_gap,
               t->entry_hit, t->tx_i, t->batch_i0, t->batch_i1,
               t->complete_rx_ns, dt);
    }
    fprintf(stderr,
            "data_shreds=%" PRIu64 " batches=%" PRIu64 " ok=%" PRIu64
            " gap=%" PRIu64 " parse_fail=%" PRIu64 "\n",
            g_shred_data, g_batches, g_batches_ok, g_batches_gap, g_parse_fail);
    for (i = 0; i < npath; i++) {
        free(paths[i]);
    }
    free(paths);
    return 0;
}
