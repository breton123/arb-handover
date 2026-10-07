/*
 * CAP-002 — read-only FEC / Entry / tx reconstruction from FEEDCAP1.
 * Uses data shreds when a FEC set is complete. Does not write the .cap.
 */
#include "feed.h"
#include "shred.h"
#include "util.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHRED_OFF_FLAGS  85u
#define SHRED_OFF_SIZE   86u
#define SHRED_OFF_N_DATA 83u
#define SHRED_OFF_N_CODE 85u
#define DATA_HDR         88u
#define DATA_COMPLETE    0x80u
#define OPEN_SLOTS       256u
#define MIN_CAP_BYTES    (1u << 20)
#define SAMPLE_MAX       2000000u

static const uint8_t VOTE_ID[32] = {
    0x07, 0x61, 0x48, 0x1d, 0x35, 0x74, 0x74, 0xbb,
    0x7c, 0x4d, 0x76, 0x24, 0xeb, 0xd3, 0xbd, 0xb3,
    0xd8, 0x35, 0x5e, 0x73, 0xd1, 0x10, 0x43, 0xfc,
    0x0d, 0xa3, 0x53, 0x80, 0x00, 0x00, 0x00, 0x00
};

typedef struct {
    uint32_t index;
    uint32_t fec;
    uint32_t arena_off;
    uint16_t plen;
    uint8_t  flags;
    uint64_t rx_ns;
} drec_t;

typedef struct {
    uint32_t fec;
    uint16_t n_data;
    uint16_t n_code;
    uint32_t data_cnt;
    uint32_t code_cnt;
    uint64_t data_ready_ns;
    uint64_t rs_ready_ns;
    uint8_t  have_params;
} fecinfo_t;

typedef struct {
    uint64_t   slot;
    uint8_t    used;
    drec_t    *d;
    uint32_t   dn;
    uint32_t   dcap;
    uint8_t   *arena;
    uint32_t   arena_n;
    uint32_t   arena_cap;
    fecinfo_t *fec;
    uint32_t   fn;
    uint32_t   fcap;
} slot_t;

typedef struct {
    uint64_t shred_data;
    uint64_t shred_code;
    uint64_t bad;
    uint64_t extract_fail;
    uint64_t slots_flushed;
    uint64_t batches;
    uint64_t batches_ok;
    uint64_t batches_gap;
    uint64_t batches_parse_fail;
    uint64_t entries;
    uint64_t entries_empty;
    uint64_t txs;
    uint64_t vote;
    uint64_t nonvote;
    uint64_t tx_skip_fail;
    uint64_t fec_ready_data;
    uint64_t fec_ready_rs;
    uint64_t early_rs_n;
    uint64_t early_rs_zero;
    uint32_t *early_us;
    uint32_t  early_n;
} recon_t;

static int
cu32(const uint8_t *p, uint32_t avail, uint32_t *val, uint32_t *used)
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
        *val = (uint32_t)(p[0] & 0x7fu) | ((uint32_t)p[1] << 7);
        *used = 2;
        return 0;
    }
    if (avail < 3 || (p[2] & 0xfcu) != 0 || p[2] == 0) {
        return -1;
    }
    *val = (uint32_t)(p[0] & 0x7fu)
         | ((uint32_t)(p[1] & 0x7fu) << 7)
         | ((uint32_t)p[2] << 14);
    *used = 3;
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
    *off += used;
    if (*off + nacc > len) {
        return -1;
    }
    *off += nacc;
    if (cu32(p + *off, len - *off, &dlen, &used) != 0) {
        return -1;
    }
    *off += used;
    if (*off + dlen > len) {
        return -1;
    }
    *off += dlen;
    return 0;
}

/* Skip one versioned/legacy tx. Returns 1 if it mentions the vote program. */
static int
skip_tx(const uint8_t *p, uint32_t len, uint32_t *off, int *is_vote)
{
    uint32_t used, nsig, nkeys, ninstr, i, versioned = 0, nlut;
    uint8_t b0;

    *is_vote = 0;
    if (cu32(p + *off, len - *off, &nsig, &used) != 0 || nsig == 0 || nsig > 64) {
        return -1;
    }
    *off += used;
    if (*off + nsig * 64u > len) {
        return -1;
    }
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
    *off += used;
    if (*off + nkeys * 32u + 32u > len) {
        return -1;
    }
    for (i = 0; i < nkeys; i++) {
        if (memcmp(p + *off + i * 32u, VOTE_ID, 32) == 0) {
            *is_vote = 1;
        }
    }
    *off += nkeys * 32u + 32u;
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
            if (*off > len) {
                return -1;
            }
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
parse_entries(const uint8_t *p, uint32_t len, recon_t *r)
{
    uint64_t nent, e;
    uint32_t off = 0;

    if (len < 8) {
        return -1;
    }
    memcpy(&nent, p, 8);
    off = 8;
    if (nent == 0 || nent > 4096) {
        return -1;
    }
    for (e = 0; e < nent; e++) {
        uint64_t ntx, i;

        if (off + 48u > len) {
            return -1;
        }
        off += 40u;
        memcpy(&ntx, p + off, 8);
        off += 8;
        if (ntx > 4096) {
            return -1;
        }
        r->entries++;
        if (ntx == 0) {
            r->entries_empty++;
            continue;
        }
        for (i = 0; i < ntx; i++) {
            int vote = 0;
            if (skip_tx(p, len, &off, &vote) != 0) {
                r->tx_skip_fail++;
                return -1;
            }
            r->txs++;
            if (vote) {
                r->vote++;
            } else {
                r->nonvote++;
            }
        }
    }
    return 0;
}

static fecinfo_t *
fec_of(slot_t *s, uint32_t fec)
{
    uint32_t i;
    fecinfo_t *n;

    for (i = 0; i < s->fn; i++) {
        if (s->fec[i].fec == fec) {
            return &s->fec[i];
        }
    }
    if (s->fn == s->fcap) {
        uint32_t cap = s->fcap == 0 ? 64 : s->fcap * 2;
        n = realloc(s->fec, cap * sizeof(*n));
        if (n == NULL) {
            return NULL;
        }
        memset(n + s->fcap, 0, (cap - s->fcap) * sizeof(*n));
        s->fec = n;
        s->fcap = cap;
    }
    n = &s->fec[s->fn++];
    memset(n, 0, sizeof(*n));
    n->fec = fec;
    return n;
}

static void
note_ready(fecinfo_t *f, uint64_t rx, recon_t *r)
{
    if (f->have_params && f->n_data != 0) {
        if (f->data_ready_ns == 0 && f->data_cnt >= f->n_data) {
            f->data_ready_ns = rx;
            r->fec_ready_data++;
        }
        if (f->rs_ready_ns == 0 && f->data_cnt + f->code_cnt >= f->n_data) {
            f->rs_ready_ns = rx;
            r->fec_ready_rs++;
        }
    }
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

static fecinfo_t *
fec_find(slot_t *s, uint32_t fec)
{
    uint32_t i;
    for (i = 0; i < s->fn; i++) {
        if (s->fec[i].fec == fec) {
            return &s->fec[i];
        }
    }
    return NULL;
}

static void
sample_early(recon_t *r, uint64_t complete_ns, uint64_t ready_ns)
{
    uint64_t d;
    if (ready_ns == 0 || complete_ns < ready_ns) {
        return;
    }
    d = complete_ns - ready_ns;
    r->early_rs_n++;
    if (d == 0) {
        r->early_rs_zero++;
    }
    if (r->early_n < SAMPLE_MAX) {
        uint64_t us = d / 1000ull;
        r->early_us[r->early_n++] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    }
}

static void
flush_slot(slot_t *s, recon_t *r)
{
    uint32_t i, run0;
    uint32_t expect;

    if (!s->used) {
        return;
    }
    r->slots_flushed++;
    if (s->dn == 0) {
        goto done;
    }
    qsort(s->d, s->dn, sizeof(drec_t), cmp_drec);
    run0 = 0;
    expect = s->d[0].index;
    for (i = 0; i < s->dn; i++) {
        int complete = (s->d[i].flags & DATA_COMPLETE) != 0;
        if (s->d[i].index != expect) {
            if (complete) {
                r->batches++;
                r->batches_gap++;
            }
            run0 = i;
            expect = s->d[i].index;
        }
        if (complete) {
            uint32_t j, blen = 0;
            uint8_t *buf;
            uint64_t complete_ns = s->d[i].rx_ns;
            uint64_t rs_ready = 0;
            int ok = 1;

            r->batches++;
            for (j = run0; j <= i; j++) {
                if (s->d[j].index != s->d[run0].index + (j - run0)) {
                    ok = 0;
                    break;
                }
                blen += s->d[j].plen;
            }
            if (!ok) {
                r->batches_gap++;
            } else {
                buf = malloc(blen == 0 ? 1 : blen);
                if (buf == NULL) {
                    goto done;
                }
                blen = 0;
                for (j = run0; j <= i; j++) {
                    fecinfo_t *f = fec_find(s, s->d[j].fec);
                    memcpy(buf + blen, s->arena + s->d[j].arena_off, s->d[j].plen);
                    blen += s->d[j].plen;
                    if (f != NULL && f->rs_ready_ns != 0) {
                        if (rs_ready < f->rs_ready_ns) {
                            rs_ready = f->rs_ready_ns;
                        }
                    }
                }
                if (parse_entries(buf, blen, r) == 0) {
                    r->batches_ok++;
                    sample_early(r, complete_ns, rs_ready);
                } else {
                    r->batches_parse_fail++;
                }
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
    free(s->fec);
    memset(s, 0, sizeof(*s));
}

static slot_t *
slot_get(slot_t *tab, recon_t *r, uint64_t slot)
{
    uint32_t i, empty = OPEN_SLOTS;
    uint32_t oldest = 0;
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
        flush_slot(&tab[oldest], r);
        empty = oldest;
    }
    tab[empty].used = 1;
    tab[empty].slot = slot;
    return &tab[empty];
}

static int
add_data(slot_t *s, const shred_view_t *v, const uint8_t *pkt, uint16_t plen,
         uint64_t rx, recon_t *r)
{
    uint16_t size, pay;
    drec_t *nd;
    fecinfo_t *f;

    if (plen < DATA_HDR + 1u) {
        r->extract_fail++;
        return 0;
    }
    size = shred_load_u16_le(pkt + SHRED_OFF_SIZE);
    if (size < DATA_HDR || size > plen) {
        r->extract_fail++;
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
    nd->flags = pkt[SHRED_OFF_FLAGS];
    nd->rx_ns = rx;
    s->arena_n += pay;
    f = fec_of(s, v->fec_set);
    if (f == NULL) {
        return -1;
    }
    f->data_cnt++;
    note_ready(f, rx, r);
    return 0;
}

static int
add_code(slot_t *s, const shred_view_t *v, const uint8_t *pkt, uint16_t plen,
         uint64_t rx, recon_t *r)
{
    fecinfo_t *f;

    if (plen < 89) {
        return 0;
    }
    f = fec_of(s, v->fec_set);
    if (f == NULL) {
        return -1;
    }
    if (!f->have_params) {
        f->n_data = shred_load_u16_le(pkt + SHRED_OFF_N_DATA);
        f->n_code = shred_load_u16_le(pkt + SHRED_OFF_N_CODE);
        f->have_params = 1;
    }
    f->code_cnt++;
    note_ready(f, rx, r);
    return 0;
}

static int
cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t
pct(const uint32_t *v, uint32_t n, unsigned p)
{
    uint64_t i;
    if (n == 0) {
        return 0;
    }
    i = ((uint64_t)(p) * (n - 1u)) / 100u;
    return v[i];
}

static int
walk_file(slot_t *tab, recon_t *r, const char *path)
{
    FILE *f;
    feedcap_hdr_t hdr;
    feed_slot_t slot;
    long sz;

    f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < (long)(FEEDCAP_HDR_LEN + MIN_CAP_BYTES)) {
        fclose(f);
        return 0;
    }
    rewind(f);
    if (feedcap_read_header(f, &hdr) != 0) {
        fclose(f);
        return -1;
    }
    fprintf(stderr, "read  %s\n", path);
    for (;;) {
        shred_view_t v;
        slot_t *s;
        int rc = feedcap_read_slot(f, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fclose(f);
            return -1;
        }
        if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
            r->bad++;
            continue;
        }
        s = slot_get(tab, r, v.slot);
        if (shred_is_data(v.type)) {
            r->shred_data++;
            if (add_data(s, &v, slot.data, (uint16_t)slot.len, slot.rx_ns, r) != 0) {
                fclose(f);
                return -1;
            }
        } else {
            r->shred_code++;
            if (add_code(s, &v, slot.data, (uint16_t)slot.len, slot.rx_ns, r) != 0) {
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    return 0;
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int
walk_dir(slot_t *tab, recon_t *r, const char *dir)
{
    DIR *d;
    struct dirent *ent;
    char **paths = NULL;
    uint32_t n = 0, cap = 0, i;
    int rc = 0;

    d = opendir(dir);
    if (d == NULL) {
        perror(dir);
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
            if (walk_file(tab, r, paths[i]) != 0) {
                rc = -1;
                break;
            }
        }
    }
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
    recon_t r;
    const char *dir = NULL;
    uint32_t i;

    memset(tab, 0, sizeof(tab));
    memset(&r, 0, sizeof(r));
    r.early_us = calloc(SAMPLE_MAX, sizeof(uint32_t));
    if (r.early_us == NULL) {
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "--dir") == 0) {
        dir = argv[2];
    } else {
        fprintf(stderr, "usage: %s --dir DIR\n", argv[0]);
        return 1;
    }
    if (walk_dir(tab, &r, dir) != 0) {
        return 1;
    }
    for (i = 0; i < OPEN_SLOTS; i++) {
        flush_slot(&tab[i], &r);
    }
    qsort(r.early_us, r.early_n, sizeof(uint32_t), cmp_u32);
    printf("CAP-002  FEC / ENTRY / TX\n");
    printf("data_shreds        %" PRIu64 "\n", r.shred_data);
    printf("coding_shreds      %" PRIu64 "\n", r.shred_code);
    printf("bad                %" PRIu64 "\n", r.bad);
    printf("extract_fail       %" PRIu64 "\n", r.extract_fail);
    printf("slots_flushed      %" PRIu64 "\n", r.slots_flushed);
    printf("fec_data_ready     %" PRIu64 "\n", r.fec_ready_data);
    printf("fec_rs_ready       %" PRIu64 "\n", r.fec_ready_rs);
    printf("batches            %" PRIu64 "\n", r.batches);
    printf("batches_ok         %" PRIu64 "\n", r.batches_ok);
    printf("batches_gap        %" PRIu64 "\n", r.batches_gap);
    printf("batches_parse_fail %" PRIu64 "\n", r.batches_parse_fail);
    printf("entries            %" PRIu64 "\n", r.entries);
    printf("entries_empty      %" PRIu64 "\n", r.entries_empty);
    printf("txs                %" PRIu64 "\n", r.txs);
    printf("vote               %" PRIu64 "\n", r.vote);
    printf("nonvote            %" PRIu64 "\n", r.nonvote);
    printf("tx_skip_fail       %" PRIu64 "\n", r.tx_skip_fail);
    printf("early_rs_samples   %" PRIu64 "\n", r.early_rs_n);
    printf("early_rs_zero      %" PRIu64 "\n", r.early_rs_zero);
    printf("rs_before_complete_us  p10=%u p50=%u p90=%u p99=%u\n",
           pct(r.early_us, r.early_n, 10),
           pct(r.early_us, r.early_n, 50),
           pct(r.early_us, r.early_n, 90),
           pct(r.early_us, r.early_n, 99));
    if (r.batches != 0) {
        printf("batch_ok%%          %.3f\n",
               100.0 * (double)r.batches_ok / (double)r.batches);
    }
    free(r.early_us);
    return 0;
}
