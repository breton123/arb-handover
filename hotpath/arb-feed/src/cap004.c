/*
 * CAP-004 — locate Mriya + trigger N in frozen FEEDCAP1 files.
 * Read-only. Does not rewrite the .cap.
 */
#include "feed.h"
#include "shred.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHRED_OFF_SIZE 86u
#define DATA_HDR       88u
#define OPEN_SLOTS     256u
#define MIN_CAP_BYTES  (1u << 20)
#define PAIR_MAX       200000u

typedef struct {
    uint32_t index;
    uint32_t fec;
    uint32_t arena_off;
    uint16_t plen;
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
    uint64_t slot;
    uint32_t tick;
    int32_t  dist;
    uint32_t trigger_i;
    uint32_t mriya_i;
    uint8_t  immediate;
    uint8_t  pair_ok;
    char     mriya_sig[128];
    char     trigger_sig[128];
    uint8_t *mriya_tx;
    uint32_t mriya_tx_len;
    uint8_t *trig_tx;
    uint32_t trig_tx_len;
    uint32_t act_off;
    uint8_t  mriya_sigb[64];
    uint8_t  trig_sigb[64];
    int      have_mriya_sig;
    int      have_trig_sig;
} pair_t;

typedef struct {
    int      found;
    uint32_t shred0;
    uint32_t shred1;
    uint32_t fec0;
    uint32_t fec1;
    uint64_t rx0;
    uint64_t rx1;
} hit_t;

static pair_t *g_pair;
static uint32_t g_npair;
static FILE *g_out;

static int
cmp_pair_slot(const void *a, const void *b)
{
    const pair_t *x = a, *y = b;
    if (x->slot < y->slot) {
        return -1;
    }
    if (x->slot > y->slot) {
        return 1;
    }
    return 0;
}

static void
pair_range(uint64_t slot, uint32_t *lo, uint32_t *hi)
{
    uint32_t l = 0, r = g_npair;
    while (l < r) {
        uint32_t m = l + (r - l) / 2u;
        if (g_pair[m].slot < slot) {
            l = m + 1u;
        } else {
            r = m;
        }
    }
    *lo = l;
    while (l < g_npair && g_pair[l].slot == slot) {
        l++;
    }
    *hi = l;
}

static int
hexnib(int c)
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
unhex(const char *s, uint8_t *out, uint32_t want)
{
    uint32_t i;
    if (s == NULL || strlen(s) < want * 2u) {
        return -1;
    }
    for (i = 0; i < want; i++) {
        int a = hexnib(s[i * 2u]);
        int b = hexnib(s[i * 2u + 1u]);
        if (a < 0 || b < 0) {
            return -1;
        }
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static char *
json_str(const char *line, const char *key)
{
    const char *p;
    char pat[80];
    size_t n;
    char *out;
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(line, pat);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(pat);
    n = strcspn(p, "\"");
    out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

static int64_t
json_i64(const char *line, const char *key, int64_t def)
{
    const char *p;
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(line, pat);
    if (p == NULL) {
        return def;
    }
    p += strlen(pat);
    if (*p == 'n') {
        return def;
    }
    if (*p == 't') {
        return 1;
    }
    if (*p == 'f') {
        return 0;
    }
    return strtoll(p, NULL, 10);
}

static int
load_pairs(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *line = NULL;
    size_t cap = 0;

    if (f == NULL) {
        perror(path);
        return -1;
    }
    g_pair = calloc(PAIR_MAX, sizeof(*g_pair));
    if (g_pair == NULL) {
        fprintf(stderr, "pairs  alloc failed\n");
        fclose(f);
        return -1;
    }
    while (g_npair < PAIR_MAX) {
        ssize_t n;
        pair_t *p;
        char *hx;
        n = getline(&line, &cap, f);
        if (n <= 0) {
            break;
        }
        p = &g_pair[g_npair];
        memset(p, 0, sizeof(*p));
        p->slot = (uint64_t)json_i64(line, "slot", 0);
        p->tick = (uint32_t)json_i64(line, "tick", 0);
        p->dist = (int32_t)json_i64(line, "dist", -1);
        p->trigger_i = (uint32_t)json_i64(line, "trigger_i", 0);
        p->mriya_i = (uint32_t)json_i64(line, "mriya_i", 0);
        p->immediate = (uint8_t)json_i64(line, "immediate", 0);
        p->pair_ok = (uint8_t)json_i64(line, "pair_ok", 0);
        p->act_off = (uint32_t)json_i64(line, "actionable_off", 0);
        hx = json_str(line, "mriya_sig");
        if (hx) {
            snprintf(p->mriya_sig, sizeof(p->mriya_sig), "%s", hx);
            free(hx);
        }
        hx = json_str(line, "trigger_sig");
        if (hx) {
            snprintf(p->trigger_sig, sizeof(p->trigger_sig), "%s", hx);
            free(hx);
        }
        hx = json_str(line, "mriya_tx_hex");
        if (hx && hx[0]) {
            p->mriya_tx_len = (uint32_t)(strlen(hx) / 2u);
            p->mriya_tx = malloc(p->mriya_tx_len);
            if (p->mriya_tx && unhex(hx, p->mriya_tx, p->mriya_tx_len) == 0) {
                /* ok */
            } else {
                free(p->mriya_tx);
                p->mriya_tx = NULL;
                p->mriya_tx_len = 0;
            }
        }
        free(hx);
        hx = json_str(line, "trigger_tx_hex");
        if (hx && hx[0]) {
            p->trig_tx_len = (uint32_t)(strlen(hx) / 2u);
            p->trig_tx = malloc(p->trig_tx_len);
            if (p->trig_tx && unhex(hx, p->trig_tx, p->trig_tx_len) == 0) {
                /* ok */
            } else {
                free(p->trig_tx);
                p->trig_tx = NULL;
                p->trig_tx_len = 0;
            }
        }
        free(hx);
        hx = json_str(line, "mriya_sig_hex");
        if (hx && unhex(hx, p->mriya_sigb, 64) == 0) {
            p->have_mriya_sig = 1;
        }
        free(hx);
        hx = json_str(line, "trigger_sig_hex");
        if (hx && unhex(hx, p->trig_sigb, 64) == 0) {
            p->have_trig_sig = 1;
        }
        free(hx);
        g_npair++;
    }
    free(line);
    fclose(f);
    if (g_npair) {
        qsort(g_pair, g_npair, sizeof(*g_pair), cmp_pair_slot);
    }
    fprintf(stderr, "pairs  %u\n", g_npair);
    return 0;
}

static const uint8_t *
find_bytes(const uint8_t *hay, uint32_t hlen, const uint8_t *ndl, uint32_t nlen)
{
    uint32_t i;
    if (ndl == NULL || nlen == 0 || hlen < nlen) {
        return NULL;
    }
    for (i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == ndl[0] && memcmp(hay + i, ndl, nlen) == 0) {
            return hay + i;
        }
    }
    return NULL;
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

static hit_t
map_range(slot_t *s, uint32_t off0, uint32_t off1)
{
    hit_t h;
    uint32_t i, acc = 0;
    memset(&h, 0, sizeof(h));
    for (i = 0; i < s->dn; i++) {
        uint32_t a = acc;
        uint32_t b = acc + s->d[i].plen;
        if (!h.found && off0 >= a && off0 < b) {
            h.found = 1;
            h.shred0 = s->d[i].index;
            h.fec0 = s->d[i].fec;
            h.rx0 = s->d[i].rx_ns;
        }
        if (h.found && off1 >= a && off1 < b) {
            h.shred1 = s->d[i].index;
            h.fec1 = s->d[i].fec;
            h.rx1 = s->d[i].rx_ns;
            return h;
        }
        acc = b;
    }
    return h;
}

static hit_t
locate(slot_t *s, const uint8_t *tx, uint32_t txlen, const uint8_t *sigb, int have_sig)
{
    hit_t h;
    const uint8_t *p;
    uint32_t off;
    memset(&h, 0, sizeof(h));
    if (tx && txlen) {
        p = find_bytes(s->arena, s->arena_n, tx, txlen);
        if (p != NULL) {
            off = (uint32_t)(p - s->arena);
            return map_range(s, off, off + txlen - 1u);
        }
    }
    if (have_sig) {
        p = find_bytes(s->arena, s->arena_n, sigb, 64);
        if (p != NULL) {
            off = (uint32_t)(p - s->arena);
            return map_range(s, off, off + 63u);
        }
    }
    return h;
}

static hit_t
locate_act(slot_t *s, const uint8_t *tx, uint32_t txlen, uint32_t act_off)
{
    hit_t h;
    const uint8_t *p;
    uint32_t off, last;
    memset(&h, 0, sizeof(h));
    if (tx == NULL || txlen == 0) {
        return h;
    }
    p = find_bytes(s->arena, s->arena_n, tx, txlen);
    if (p == NULL) {
        return h;
    }
    off = (uint32_t)(p - s->arena);
    last = act_off == 0 || act_off > txlen ? txlen : act_off;
    if (last == 0) {
        last = 1;
    }
    return map_range(s, off, off + last - 1u);
}

static void
emit(pair_t *p, hit_t *m, hit_t *t, hit_t *ta)
{
    fprintf(g_out,
            "{\"mriya_sig\":\"%s\",\"trigger_sig\":\"%s\",\"slot\":%" PRIu64
            ",\"tick\":%u,\"dist\":%d,\"immediate\":%u,\"pair_ok\":%u,"
            "\"mriya_i\":%u,\"trigger_i\":%u,"
            "\"mriya_found\":%u,\"mriya_shred0\":%u,\"mriya_shred1\":%u,"
            "\"mriya_fec0\":%u,\"mriya_fec1\":%u,"
            "\"mriya_rx0\":%" PRIu64 ",\"mriya_rx1\":%" PRIu64 ","
            "\"trig_found\":%u,\"trig_shred0\":%u,\"trig_shred1\":%u,"
            "\"trig_fec0\":%u,\"trig_fec1\":%u,"
            "\"trig_rx0\":%" PRIu64 ",\"trig_rx1\":%" PRIu64 ","
            "\"act_found\":%u,\"act_shred\":%u,\"act_fec\":%u,"
            "\"act_rx\":%" PRIu64 "}\n",
            p->mriya_sig, p->trigger_sig, p->slot, p->tick, p->dist,
            (unsigned)p->immediate, (unsigned)p->pair_ok,
            p->mriya_i, p->trigger_i,
            (unsigned)m->found, m->shred0, m->shred1, m->fec0, m->fec1, m->rx0, m->rx1,
            (unsigned)t->found, t->shred0, t->shred1, t->fec0, t->fec1, t->rx0, t->rx1,
            (unsigned)ta->found, ta->shred1, ta->fec1, ta->rx1);
}

static void
flush_slot(slot_t *s)
{
    uint32_t i;
    if (!s->used) {
        return;
    }
    if (s->dn) {
        qsort(s->d, s->dn, sizeof(drec_t), cmp_drec);
        /* rebuild arena in index order */
        {
            uint8_t *na = malloc(s->arena_n ? s->arena_n : 1);
            uint32_t off = 0;
            if (na == NULL) {
                goto done;
            }
            for (i = 0; i < s->dn; i++) {
                memcpy(na + off, s->arena + s->d[i].arena_off, s->d[i].plen);
                s->d[i].arena_off = off;
                off += s->d[i].plen;
            }
            free(s->arena);
            s->arena = na;
            s->arena_n = off;
        }
        {
            uint32_t lo, hi;
            pair_range(s->slot, &lo, &hi);
            for (i = lo; i < hi; i++) {
                hit_t m, t, ta;
                m = locate(s, g_pair[i].mriya_tx, g_pair[i].mriya_tx_len,
                           g_pair[i].mriya_sigb, g_pair[i].have_mriya_sig);
                t = locate(s, g_pair[i].trig_tx, g_pair[i].trig_tx_len,
                           g_pair[i].trig_sigb, g_pair[i].have_trig_sig);
                ta = locate_act(s, g_pair[i].trig_tx, g_pair[i].trig_tx_len,
                                g_pair[i].act_off);
                emit(&g_pair[i], &m, &t, &ta);
            }
        }
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
add_data(slot_t *s, const shred_view_t *v, const uint8_t *pkt, uint16_t plen, uint64_t rx)
{
    uint16_t size, pay;
    drec_t *nd;
    if (plen < DATA_HDR) {
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
        uint32_t cap = s->arena_cap == 0 ? 1u << 16 : s->arena_cap;
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
    nd = &s->d[s->dn++];
    nd->index = v->index;
    nd->fec = v->fec_set;
    nd->arena_off = s->arena_n;
    nd->plen = pay;
    nd->rx_ns = rx;
    s->arena_n += pay;
    return 0;
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int
walk_dir(slot_t *tab, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    char **paths = NULL;
    uint32_t n = 0, cap = 0, i;
    int rc = 0;
    if (d == NULL) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        char *p;
        if (strstr(ent->d_name, "shredstream-") != ent->d_name) {
            continue;
        }
        if (len < 4 || strcmp(ent->d_name + len - 4, ".cap") != 0) {
            continue;
        }
        if (n == cap) {
            uint32_t ncap = cap == 0 ? 64 : cap * 2;
            char **np = realloc(paths, ncap * sizeof(*np));
            if (np == NULL) {
                rc = -1;
                break;
            }
            paths = np;
            cap = ncap;
        }
        p = malloc(strlen(dir) + 1 + len + 1);
        if (p == NULL) {
            rc = -1;
            break;
        }
        sprintf(p, "%s/%s", dir, ent->d_name);
        paths[n++] = p;
    }
    closedir(d);
    if (rc == 0) {
        qsort(paths, n, sizeof(*paths), cmp_str);
        for (i = 0; i < n; i++) {
            FILE *f;
            feedcap_hdr_t hdr;
            feed_slot_t slot;
            long sz;
            f = fopen(paths[i], "rb");
            if (f == NULL) {
                continue;
            }
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            if (sz < (long)(FEEDCAP_HDR_LEN + MIN_CAP_BYTES)) {
                fclose(f);
                continue;
            }
            rewind(f);
            if (feedcap_read_header(f, &hdr) != 0) {
                fclose(f);
                continue;
            }
            fprintf(stderr, "read  %s\n", paths[i]);
            for (;;) {
                shred_view_t v;
                int r = feedcap_read_slot(f, &slot);
                if (r == 1) {
                    break;
                }
                if (r != 0) {
                    break;
                }
                if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
                    continue;
                }
                if (!shred_is_data(v.type)) {
                    continue;
                }
                if (add_data(slot_get(tab, v.slot), &v, slot.data,
                             (uint16_t)slot.len, slot.rx_ns) != 0) {
                    fclose(f);
                    rc = -1;
                    goto out;
                }
            }
            fclose(f);
        }
    }
out:
    for (i = 0; i < n; i++) {
        free(paths[i]);
    }
    free(paths);
    return rc;
}

int
main(int argc, char **argv)
{
    slot_t tab[OPEN_SLOTS];
    const char *dir = NULL;
    const char *pairs = NULL;
    const char *outp = NULL;
    uint32_t i;

    memset(tab, 0, sizeof(tab));
    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < (uint32_t)argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--pairs") == 0 && i + 1 < (uint32_t)argc) {
            pairs = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < (uint32_t)argc) {
            outp = argv[++i];
        }
    }
    if (dir == NULL || pairs == NULL || outp == NULL) {
        fprintf(stderr, "usage: %s --dir DIR --pairs pairs.jsonl --out hits.jsonl\n", argv[0]);
        return 1;
    }
    if (load_pairs(pairs) != 0) {
        return 1;
    }
    g_out = fopen(outp, "wb");
    if (g_out == NULL) {
        perror(outp);
        return 1;
    }
    if (walk_dir(tab, dir) != 0) {
        return 1;
    }
    for (i = 0; i < OPEN_SLOTS; i++) {
        flush_slot(&tab[i]);
    }
    fclose(g_out);
    return 0;
}
