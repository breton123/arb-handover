/*
 * CORE-010 replay — early framing vs commit on opp_synced N.
 * Read-only. Does not write .cap. Does not send.
 */
#include "feed.h"
#include "frame.h"
#include "shred.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SHRED_OFF_FLAGS  85u
#define SHRED_OFF_SIZE   86u
#define SHRED_OFF_N_DATA 83u
#define SHRED_OFF_N_CODE 85u
#define DATA_HDR         88u
#define DATA_COMPLETE    0x80u
#define OPEN_SLOTS       256u
#define TGT_MAX          512u
#define HT               2048u
#define SHRED_CAP        2048u
#define FEC_CAP          128u
#define BUF_CAP          (2u * 1024u * 1024u)

typedef struct {
    uint32_t index, fec;
    uint16_t plen;
    uint8_t  flags;
    uint64_t rx_ns;
    uint8_t *pay;
} srec_t;

typedef struct {
    uint32_t fec, n_data, n_code, data_cnt, code_cnt;
    uint64_t rs_ready_ns;
} fec_t;

typedef struct {
    uint64_t slot;
    uint8_t  used;
    srec_t   d[SHRED_CAP];
    uint32_t dn;
    fec_t    fec[FEC_CAP];
    uint32_t fn;
} slot_t;

typedef struct {
    uint8_t  sig[64];
    char     hex[129];
    int      hit;
    uint64_t slot;
    uint32_t index, fec;
    uint64_t rx_ns;
    uint32_t pay_off;
} tgt_t;

static tgt_t g_tgt[TGT_MAX];
static uint32_t g_ntgt;
static int16_t g_ht[HT];
static uint8_t g_buf[BUF_CAP];

static const char *g_name[] = { "invalid", "incomplete", "framed" };

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
            int hi = hex_nib(line[i * 2u]), lo = hex_nib(line[i * 2u + 1u]);
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
}

static fec_t *
fec_of(slot_t *s, uint32_t fec)
{
    uint32_t i;
    for (i = 0; i < s->fn; i++) {
        if (s->fec[i].fec == fec) {
            return &s->fec[i];
        }
    }
    if (s->fn >= FEC_CAP) {
        return NULL;
    }
    memset(&s->fec[s->fn], 0, sizeof(s->fec[0]));
    s->fec[s->fn].fec = fec;
    return &s->fec[s->fn++];
}

static srec_t *
find_idx(slot_t *s, uint32_t idx)
{
    uint32_t i;
    for (i = 0; i < s->dn; i++) {
        if (s->d[i].index == idx) {
            return &s->d[i];
        }
    }
    return NULL;
}

static uint32_t
concat(slot_t *s, uint32_t i0, uint32_t i1, uint64_t *rx_last)
{
    uint32_t n = 0, idx;
    *rx_last = 0;
    for (idx = i0; idx <= i1; idx++) {
        srec_t *r = find_idx(s, idx);
        if (r == NULL) {
            return 0;
        }
        if (n + r->plen > BUF_CAP) {
            return 0;
        }
        memcpy(g_buf + n, r->pay, r->plen);
        n += r->plen;
        if (r->rx_ns > *rx_last) {
            *rx_last = r->rx_ns;
        }
    }
    return n;
}

static int
cmp_srec(const void *a, const void *b)
{
    const srec_t *x = *(srec_t *const *)a, *y = *(srec_t *const *)b;
    return (x->index > y->index) - (x->index < y->index);
}

static uint32_t
concat_fec(slot_t *s, uint32_t fec, uint64_t before, uint64_t *rx_last)
{
    srec_t *ord[SHRED_CAP];
    uint32_t i, m = 0, n = 0;
    *rx_last = 0;
    for (i = 0; i < s->dn; i++) {
        if (s->d[i].fec != fec) {
            continue;
        }
        if (before != 0 && s->d[i].rx_ns > before) {
            continue;
        }
        ord[m++] = &s->d[i];
    }
    qsort(ord, m, sizeof(ord[0]), cmp_srec);
    for (i = 0; i < m; i++) {
        if (n + ord[i]->plen > BUF_CAP) {
            return 0;
        }
        memcpy(g_buf + n, ord[i]->pay, ord[i]->plen);
        n += ord[i]->plen;
        if (ord[i]->rx_ns > *rx_last) {
            *rx_last = ord[i]->rx_ns;
        }
    }
    return n;
}

static void
emit_cls(const char *name, int have, int klass, int64_t us)
{
    printf(",\"%s\":{\"have\":%d,\"class\":\"%s\",\"us\":%" PRId64 "}",
           name, have, have ? g_name[klass] : "incomplete", have ? us : 0);
}

static void
eval_tgt(slot_t *s, tgt_t *t)
{
    frame_hit_t h;
    uint64_t rx;
    uint32_t n, i;
    int k;
    int64_t us;
    fec_t *f = NULL;
    uint32_t complete = 0;
    int have_c = 0;

    printf("{\"sig_hex\":\"%s\",\"slot\":%" PRIu64 ",\"index\":%u,\"fec\":%u",
           t->hex, t->slot, t->index, t->fec);

    n = concat(s, t->index, t->index, &rx);
    k = (n >= 64) ? frame_around_sig(g_buf, n, t->sig, &h) : FRAME_INCOMPLETE;
    emit_cls("t0", n >= 64, k, 0);

    n = concat(s, 0, t->index, &rx);
    if (n == 0) {
        uint32_t start = t->index;
        while (start > 0 && find_idx(s, start - 1u) != NULL) {
            start--;
        }
        n = concat(s, start, t->index, &rx);
    }
    k = (n >= 64) ? frame_around_sig(g_buf, n, t->sig, &h) : FRAME_INCOMPLETE;
    us = (rx > t->rx_ns) ? (int64_t)((rx - t->rx_ns) / 1000ull) : 0;
    emit_cls("t0_prefix", n >= 64, k, us);

    for (i = 1; i <= 2; i++) {
        char nm[8];
        n = concat(s, t->index, t->index + i, &rx);
        k = (n >= 64) ? frame_around_sig(g_buf, n, t->sig, &h)
            : FRAME_INCOMPLETE;
        us = (n >= 64 && rx > t->rx_ns)
            ? (int64_t)((rx - t->rx_ns) / 1000ull) : 0;
        snprintf(nm, sizeof(nm), "t%u", i);
        emit_cls(nm, n >= 64 && find_idx(s, t->index + i) != NULL, k, us);
    }

    for (i = 0; i < s->fn; i++) {
        if (s->fec[i].fec == t->fec) {
            f = &s->fec[i];
            break;
        }
    }
    {
        uint32_t want = (f != NULL && f->n_data != 0) ? f->n_data : 32;
        uint32_t got = 0;
        for (i = 0; i < s->dn; i++) {
            if (s->d[i].fec == t->fec) {
                got++;
            }
        }
        n = concat_fec(s, t->fec, 0, &rx);
        k = (n >= 64) ? frame_around_sig(g_buf, n, t->sig, &h)
            : FRAME_INCOMPLETE;
        us = (rx > t->rx_ns) ? (int64_t)((rx - t->rx_ns) / 1000ull) : 0;
        emit_cls("fec", got >= want, k, us);
        if (f != NULL && f->rs_ready_ns != 0) {
            n = concat_fec(s, t->fec, f->rs_ready_ns, &rx);
            k = (n >= 64) ? frame_around_sig(g_buf, n, t->sig, &h)
                : FRAME_INCOMPLETE;
            us = (f->rs_ready_ns > t->rx_ns)
                ? (int64_t)((f->rs_ready_ns - t->rx_ns) / 1000ull) : 0;
            emit_cls("rs", 1, k, us);
        } else {
            emit_cls("rs", 0, FRAME_INCOMPLETE, 0);
        }
    }

    for (i = 0; i < s->dn; i++) {
        if ((s->d[i].flags & DATA_COMPLETE) != 0 && s->d[i].index >= t->index) {
            complete = s->d[i].index;
            have_c = 1;
            break;
        }
    }
    if (have_c) {
        n = concat(s, 0, complete, &rx);
        if (n == 0) {
            uint32_t start = t->index;
            while (start > 0 && find_idx(s, start - 1u) != NULL) {
                start--;
            }
            n = concat(s, start, complete, &rx);
        }
        k = (n >= 64) ? frame_around_sig(g_buf, n, t->sig, &h)
            : FRAME_INCOMPLETE;
        us = (rx > t->rx_ns) ? (int64_t)((rx - t->rx_ns) / 1000ull) : 0;
        emit_cls("slot", n >= 64, k, us);
    } else {
        emit_cls("slot", 0, FRAME_INCOMPLETE, 0);
    }
    printf("}\n");
}

static void
flush_slot(slot_t *s)
{
    uint32_t t, i;
    if (!s->used) {
        return;
    }
    for (t = 0; t < g_ntgt; t++) {
        if (g_tgt[t].hit && g_tgt[t].slot == s->slot) {
            eval_tgt(s, &g_tgt[t]);
        }
    }
    for (i = 0; i < s->dn; i++) {
        free(s->d[i].pay);
    }
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

static void
note_sig(const uint8_t *pay, uint16_t payn, const shred_view_t *v, uint64_t rx)
{
    uint32_t off;
    if (payn < 64) {
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
            if (tk == k && !g_tgt[idx].hit
                && memcmp(pay + off, g_tgt[idx].sig, 64) == 0) {
                g_tgt[idx].hit = 1;
                g_tgt[idx].slot = v->slot;
                g_tgt[idx].index = v->index;
                g_tgt[idx].fec = v->fec_set;
                g_tgt[idx].rx_ns = rx;
                g_tgt[idx].pay_off = off;
            }
            h = (h + 1u) & (HT - 1u);
        }
    }
}

static int
add_data(slot_t *s, const shred_view_t *v, const uint8_t *pkt, uint16_t plen,
         uint64_t rx)
{
    uint16_t size, pay;
    srec_t *r;
    fec_t *f;
    if (plen < DATA_HDR + 1u || s->dn >= SHRED_CAP) {
        return 0;
    }
    size = shred_load_u16_le(pkt + SHRED_OFF_SIZE);
    if (size < DATA_HDR || size > plen) {
        return 0;
    }
    pay = (uint16_t)(size - DATA_HDR);
    r = &s->d[s->dn];
    r->pay = malloc(pay == 0 ? 1 : pay);
    if (r->pay == NULL) {
        return -1;
    }
    memcpy(r->pay, pkt + DATA_HDR, pay);
    r->index = v->index;
    r->fec = v->fec_set;
    r->plen = pay;
    r->flags = pkt[SHRED_OFF_FLAGS];
    r->rx_ns = rx;
    s->dn++;
    note_sig(pkt + DATA_HDR, pay, v, rx);
    f = fec_of(s, v->fec_set);
    if (f != NULL) {
        f->data_cnt++;
        if (f->n_data != 0
            && f->rs_ready_ns == 0
            && f->data_cnt + f->code_cnt >= f->n_data) {
            f->rs_ready_ns = rx;
        }
    }
    return 0;
}

static int
add_code(slot_t *s, const shred_view_t *v, const uint8_t *pkt, uint16_t plen,
         uint64_t rx)
{
    fec_t *f;
    if (plen < 89) {
        return 0;
    }
    f = fec_of(s, v->fec_set);
    if (f == NULL) {
        return 0;
    }
    if (f->n_data == 0) {
        f->n_data = shred_load_u16_le(pkt + SHRED_OFF_N_DATA);
        f->n_code = shred_load_u16_le(pkt + SHRED_OFF_N_CODE);
    }
    f->code_cnt++;
    if (f->n_data != 0 && f->rs_ready_ns == 0
        && f->data_cnt + f->code_cnt >= f->n_data) {
        f->rs_ready_ns = rx;
    }
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
        return 0;
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
        if (feedcap_read_slot(f, &slot) != 0) {
            break;
        }
        if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
            continue;
        }
        s = slot_get(tab, v.slot);
        if (shred_is_data(v.type)) {
            if (add_data(s, &v, slot.data, (uint16_t)slot.len, slot.rx_ns) != 0) {
                break;
            }
        } else if (shred_is_code(v.type)) {
            (void)add_code(s, &v, slot.data, (uint16_t)slot.len, slot.rx_ns);
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
    return (ta > tb) - (ta < tb);
}

static int
list_caps(const char *dir, uint32_t newest, char ***out, uint32_t *n)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    char **paths = NULL;
    uint32_t cap = 0, i;
    *out = NULL;
    *n = 0;
    if (d == NULL) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t L = strlen(ent->d_name);
        char *p;
        if (L < 5 || strcmp(ent->d_name + L - 4, ".cap") != 0
            || strncmp(ent->d_name, "orbitflare-", 11) != 0) {
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

int
main(int argc, char **argv)
{
    static slot_t tab[OPEN_SLOTS];
    const char *dir = NULL, *sigs = NULL;
    uint32_t newest = 20, i;
    char **paths = NULL;
    uint32_t npath = 0;

    memset(tab, 0, sizeof(tab));
    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < (uint32_t)argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--sigs") == 0 && i + 1 < (uint32_t)argc) {
            sigs = argv[++i];
        } else if (strcmp(argv[i], "--newest") == 0 && i + 1 < (uint32_t)argc) {
            newest = (uint32_t)atoi(argv[++i]);
        }
    }
    if (dir == NULL || sigs == NULL || load_sigs(sigs) != 0 || g_ntgt == 0) {
        fprintf(stderr, "usage: %s --dir DIR --sigs HEX [--newest N]\n", argv[0]);
        return 1;
    }
    ht_build();
    fprintf(stderr, "CORE-010  targets=%u  newest=%u  NO SEND\n", g_ntgt, newest);
    if (list_caps(dir, newest, &paths, &npath) != 0) {
        return 1;
    }
    for (i = 0; i < npath; i++) {
        (void)walk_file(tab, paths[i]);
    }
    for (i = 0; i < OPEN_SLOTS; i++) {
        flush_slot(&tab[i]);
    }
    for (i = 0; i < g_ntgt; i++) {
        if (!g_tgt[i].hit) {
            printf("{\"sig_hex\":\"%s\",\"t0\":{\"have\":0,\"class\":\"incomplete\",\"us\":0}}\n",
                   g_tgt[i].hex);
        }
    }
    for (i = 0; i < npath; i++) {
        free(paths[i]);
    }
    free(paths);
    return 0;
}
