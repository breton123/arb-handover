/*
 * CAP-001 — read-only integrity/census of FEEDCAP1 files.
 * Never writes the .cap. CORE/EXEC stay frozen.
 */
#include "feed.h"
#include "shred.h"
#include "util.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHRED_OFF_FLAGS   85u
#define SHRED_OFF_N_DATA  83u
#define SHRED_OFF_N_CODE  85u
#define SHRED_OFF_POS     87u

#define DATA_COMPLETE     0x80u
#define LAST_IN_SLOT      0x40u

#define FEC_IDX_MAX       128u
#define MIN_CAP_BYTES     (1u << 20)

typedef struct {
    uint64_t slot;
    uint32_t fec;
    uint16_t n_data;
    uint16_t n_code;
    uint32_t data_seen;
    uint32_t code_seen;
    uint32_t dups;
    uint32_t overflow;
    uint64_t data_bits[2];
    uint64_t code_bits[2];
    uint8_t  used;
    uint8_t  have_params;
} fec_row_t;

typedef struct {
    uint64_t slot;
    uint32_t data_seen;
    uint32_t code_seen;
    uint32_t min_data;
    uint32_t max_data;
    uint8_t  used;
    uint8_t  saw_end;
} slot_row_t;

typedef struct {
    fec_row_t  *fec;
    uint32_t    fec_cap;
    uint32_t    fec_n;
    slot_row_t *slot;
    uint32_t    slot_cap;
    uint32_t    slot_n;

    uint64_t files;
    uint64_t recs;
    uint64_t bytes_payload;
    uint64_t bytes_file;
    uint64_t bad;
    uint64_t shred;
    uint64_t data;
    uint64_t coding;
    uint64_t seq_gap;
    uint64_t dups;
    uint64_t type_legacy_data;
    uint64_t type_merkle_data;
    uint64_t type_chained_data;
    uint64_t type_resigned_data;
    uint64_t type_legacy_code;
    uint64_t type_merkle_code;
    uint64_t type_chained_code;
    uint64_t type_resigned_code;
    uint64_t data_complete_flags;
    uint64_t last_in_slot_flags;
    uint64_t first_rx_ns;
    uint64_t last_rx_ns;
    uint64_t first_seq;
    uint64_t last_seq;
    uint64_t prev_seq;
    int      have_seq;
    uint16_t version;
    uint64_t version_mismatch;
    uint64_t realtime0_ns;
    uint64_t mono0_ns;
    uint64_t tsc_hz;
} census_t;

static uint64_t
mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static int
fec_grow(census_t *c)
{
    fec_row_t *n;
    uint32_t cap, i, mask;

    cap = c->fec_cap == 0 ? (1u << 21) : c->fec_cap << 1;
    n = calloc(cap, sizeof(*n));
    if (n == NULL) {
        return -1;
    }
    mask = cap - 1u;
    for (i = 0; i < c->fec_cap; i++) {
        uint32_t j;
        if (!c->fec[i].used) {
            continue;
        }
        j = (uint32_t)mix64(c->fec[i].slot ^ ((uint64_t)c->fec[i].fec * 0x9e3779b97f4a7c15ULL)) & mask;
        while (n[j].used) {
            j = (j + 1u) & mask;
        }
        n[j] = c->fec[i];
    }
    free(c->fec);
    c->fec = n;
    c->fec_cap = cap;
    return 0;
}

static fec_row_t *
fec_get(census_t *c, uint64_t slot, uint32_t fec)
{
    uint32_t mask, j, dist;

    if (c->fec_cap == 0 || c->fec_n * 10u >= c->fec_cap * 7u) {
        if (fec_grow(c) != 0) {
            return NULL;
        }
    }
    mask = c->fec_cap - 1u;
    j = (uint32_t)mix64(slot ^ ((uint64_t)fec * 0x9e3779b97f4a7c15ULL)) & mask;
    for (dist = 0; dist < c->fec_cap; dist++) {
        if (!c->fec[j].used) {
            c->fec[j].used = 1;
            c->fec[j].slot = slot;
            c->fec[j].fec = fec;
            c->fec_n++;
            return &c->fec[j];
        }
        if (c->fec[j].slot == slot && c->fec[j].fec == fec) {
            return &c->fec[j];
        }
        j = (j + 1u) & mask;
    }
    return NULL;
}

static int
slot_grow(census_t *c)
{
    slot_row_t *n;
    uint32_t cap, i, mask;

    cap = c->slot_cap == 0 ? (1u << 16) : c->slot_cap << 1;
    n = calloc(cap, sizeof(*n));
    if (n == NULL) {
        return -1;
    }
    mask = cap - 1u;
    for (i = 0; i < c->slot_cap; i++) {
        uint32_t j;
        if (!c->slot[i].used) {
            continue;
        }
        j = (uint32_t)mix64(c->slot[i].slot) & mask;
        while (n[j].used) {
            j = (j + 1u) & mask;
        }
        n[j] = c->slot[i];
    }
    free(c->slot);
    c->slot = n;
    c->slot_cap = cap;
    return 0;
}

static slot_row_t *
slot_get(census_t *c, uint64_t slot)
{
    uint32_t mask, j, dist;

    if (c->slot_cap == 0 || c->slot_n * 10u >= c->slot_cap * 7u) {
        if (slot_grow(c) != 0) {
            return NULL;
        }
    }
    mask = c->slot_cap - 1u;
    j = (uint32_t)mix64(slot) & mask;
    for (dist = 0; dist < c->slot_cap; dist++) {
        if (!c->slot[j].used) {
            c->slot[j].used = 1;
            c->slot[j].slot = slot;
            c->slot[j].min_data = UINT32_MAX;
            c->slot_n++;
            return &c->slot[j];
        }
        if (c->slot[j].slot == slot) {
            return &c->slot[j];
        }
        j = (j + 1u) & mask;
    }
    return NULL;
}

static int
bit_set(uint64_t bits[2], uint32_t idx)
{
    uint32_t word, off;

    if (idx >= FEC_IDX_MAX) {
        return 1;
    }
    word = idx >> 6;
    off = idx & 63u;
    if ((bits[word] >> off) & 1u) {
        return -1;
    }
    bits[word] |= 1ull << off;
    return 0;
}

static void
note_type(census_t *c, uint8_t type)
{
    switch (type & 0xf0u) {
    case SHRED_TYPE_LEGACY_DATA:    c->type_legacy_data++; break;
    case SHRED_TYPE_MERKLE_DATA:    c->type_merkle_data++; break;
    case SHRED_TYPE_CHAINED_DATA:   c->type_chained_data++; break;
    case SHRED_TYPE_RESIGNED_DATA:  c->type_resigned_data++; break;
    case SHRED_TYPE_LEGACY_CODE:    c->type_legacy_code++; break;
    case SHRED_TYPE_MERKLE_CODE:    c->type_merkle_code++; break;
    case SHRED_TYPE_CHAINED_CODE:   c->type_chained_code++; break;
    case SHRED_TYPE_RESIGNED_CODE:  c->type_resigned_code++; break;
    default: break;
    }
}

static int
observe(census_t *c, const feed_slot_t *s)
{
    shred_view_t v;
    fec_row_t *fr;
    slot_row_t *sr;
    int is_data;
    uint32_t local;
    int rc;

    c->recs++;
    c->bytes_payload += s->len;
    if (!c->have_seq) {
        c->first_seq = s->seq;
        c->prev_seq = s->seq;
        c->have_seq = 1;
    } else if (s->seq != c->prev_seq + 1u) {
        c->seq_gap++;
    }
    c->prev_seq = s->seq;
    c->last_seq = s->seq;

    if (shred_parse(s->data, (uint16_t)s->len, &v) != 0) {
        c->bad++;
        return 0;
    }
    c->shred++;
    if (c->first_rx_ns == 0 || s->rx_ns < c->first_rx_ns) {
        c->first_rx_ns = s->rx_ns;
    }
    if (s->rx_ns > c->last_rx_ns) {
        c->last_rx_ns = s->rx_ns;
    }
    if (c->version == 0) {
        c->version = v.version;
    } else if (v.version != c->version) {
        c->version_mismatch++;
    }
    note_type(c, v.type);
    is_data = shred_is_data(v.type);
    if (is_data) {
        c->data++;
        if (s->len >= 87) {
            uint8_t flags = s->data[SHRED_OFF_FLAGS];
            if (flags & DATA_COMPLETE) {
                c->data_complete_flags++;
            }
            if (flags & LAST_IN_SLOT) {
                c->last_in_slot_flags++;
            }
        }
    } else {
        c->coding++;
    }

    fr = fec_get(c, v.slot, v.fec_set);
    sr = slot_get(c, v.slot);
    if (fr == NULL || sr == NULL) {
        return -1;
    }
    if (is_data) {
        if (v.index < v.fec_set) {
            fr->overflow++;
            return 0;
        }
        local = v.index - v.fec_set;
        rc = bit_set(fr->data_bits, local);
        if (rc < 0) {
            fr->dups++;
            c->dups++;
        } else if (rc > 0) {
            fr->overflow++;
        } else {
            fr->data_seen++;
        }
        if (sr->data_seen == 0 || v.index < sr->min_data) {
            sr->min_data = v.index;
        }
        if (v.index > sr->max_data) {
            sr->max_data = v.index;
        }
        if (rc == 0) {
            sr->data_seen++;
        }
        if (s->len >= 86 && (s->data[SHRED_OFF_FLAGS] & LAST_IN_SLOT)) {
            sr->saw_end = 1;
        }
    } else {
        if (s->len >= 89) {
            uint16_t nd = shred_load_u16_le(s->data + SHRED_OFF_N_DATA);
            uint16_t nc = shred_load_u16_le(s->data + SHRED_OFF_N_CODE);
            uint16_t pos = shred_load_u16_le(s->data + SHRED_OFF_POS);
            if (!fr->have_params) {
                fr->n_data = nd;
                fr->n_code = nc;
                fr->have_params = 1;
            }
            local = pos;
            rc = bit_set(fr->code_bits, local);
            if (rc < 0) {
                fr->dups++;
                c->dups++;
            } else if (rc > 0) {
                fr->overflow++;
            } else {
                fr->code_seen++;
            }
            if (rc == 0) {
                sr->code_seen++;
            }
        }
    }
    return 0;
}

static int
walk_file(census_t *c, const char *path)
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
        fprintf(stderr, "skip  %s  (%ld B)\n", path, sz);
        return 0;
    }
    rewind(f);
    if (feedcap_read_header(f, &hdr) != 0) {
        fprintf(stderr, "bad header  %s\n", path);
        fclose(f);
        return -1;
    }
    if (c->files == 0) {
        c->realtime0_ns = hdr.realtime0_ns;
        c->mono0_ns = hdr.mono0_ns;
        c->tsc_hz = hdr.tsc_hz;
    }
    c->files++;
    c->bytes_file += (uint64_t)sz;
    fprintf(stderr, "read  %s  %ld B\n", path, sz);
    for (;;) {
        int rc = feedcap_read_slot(f, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fprintf(stderr, "bad record  %s after rec %" PRIu64 "\n", path, c->recs);
            fclose(f);
            return -1;
        }
        if (observe(c, &slot) != 0) {
            fclose(f);
            return -1;
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
walk_dir(census_t *c, const char *dir)
{
    DIR *d;
    struct dirent *ent;
    char **paths = NULL;
    uint32_t n = 0, cap = 0;
    uint32_t i;
    int rc = 0;

    d = opendir(dir);
    if (d == NULL) {
        perror(dir);
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t len;
        char *p;
        if (strstr(ent->d_name, "shredstream-") != ent->d_name) {
            continue;
        }
        len = strlen(ent->d_name);
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
        p = malloc(strlen(dir) + 1 + strlen(ent->d_name) + 1);
        if (p == NULL) {
            rc = -1;
            break;
        }
        sprintf(p, "%s/%s", dir, ent->d_name);
        paths[n++] = p;
    }
    closedir(d);
    if (rc != 0) {
        for (i = 0; i < n; i++) {
            free(paths[i]);
        }
        free(paths);
        return -1;
    }
    qsort(paths, n, sizeof(*paths), cmp_str);
    for (i = 0; i < n; i++) {
        if (walk_file(c, paths[i]) != 0) {
            rc = -1;
            break;
        }
    }
    for (i = 0; i < n; i++) {
        free(paths[i]);
    }
    free(paths);
    return rc;
}

static void
report(const census_t *c)
{
    uint64_t i;
    uint64_t fec_params = 0, fec_data_full = 0, fec_rs = 0, fec_empty_data = 0;
    uint64_t slots_ended = 0, slots_full_span = 0, holes = 0;
    uint64_t span_ns, wall0, wall1;
    double dur_s;

    for (i = 0; i < c->fec_cap; i++) {
        const fec_row_t *f = &c->fec[i];
        if (!f->used) {
            continue;
        }
        if (f->data_seen == 0) {
            fec_empty_data++;
        }
        if (!f->have_params) {
            continue;
        }
        fec_params++;
        if (f->n_data != 0 && f->data_seen >= f->n_data) {
            fec_data_full++;
        }
        if (f->n_data != 0 && f->data_seen + f->code_seen >= f->n_data) {
            fec_rs++;
        }
    }
    for (i = 0; i < c->slot_cap; i++) {
        const slot_row_t *s = &c->slot[i];
        uint32_t span;
        if (!s->used) {
            continue;
        }
        if (s->saw_end) {
            slots_ended++;
        }
        if (s->data_seen == 0 || s->min_data != 0) {
            continue;
        }
        span = s->max_data - s->min_data + 1u;
        if (s->data_seen >= span) {
            slots_full_span++;
        } else {
            holes += (uint64_t)span - s->data_seen;
        }
    }

    span_ns = c->last_rx_ns >= c->first_rx_ns ? c->last_rx_ns - c->first_rx_ns : 0;
    dur_s = (double)span_ns / 1e9;
    wall0 = c->realtime0_ns + (c->first_rx_ns - c->mono0_ns);
    wall1 = c->realtime0_ns + (c->last_rx_ns - c->mono0_ns);

    printf("CAP-001  INTEGRITY / CENSUS\n");
    printf("files              %" PRIu64 "\n", c->files);
    printf("file_bytes         %" PRIu64 "\n", c->bytes_file);
    printf("records            %" PRIu64 "\n", c->recs);
    printf("payload_bytes      %" PRIu64 "\n", c->bytes_payload);
    printf("seq_first          %" PRIu64 "\n", c->first_seq);
    printf("seq_last           %" PRIu64 "\n", c->last_seq);
    printf("seq_gaps           %" PRIu64 "\n", c->seq_gap);
    printf("bad_shreds         %" PRIu64 "\n", c->bad);
    printf("shred_ok           %" PRIu64 "\n", c->shred);
    printf("data               %" PRIu64 "\n", c->data);
    printf("coding             %" PRIu64 "\n", c->coding);
    printf("duplicates         %" PRIu64 "\n", c->dups);
    printf("dup_rate           %.6f\n", c->shred ? (double)c->dups / (double)c->shred : 0);
    printf("shred_version      %u\n", (unsigned)c->version);
    printf("version_mismatch   %" PRIu64 "\n", c->version_mismatch);
    printf("duration_s         %.3f\n", dur_s);
    printf("wall_start_ns      %" PRIu64 "\n", wall0);
    printf("wall_end_ns        %" PRIu64 "\n", wall1);
    printf("unique_slots       %" PRIu64 "\n", (uint64_t)c->slot_n);
    printf("slots_saw_end      %" PRIu64 "\n", slots_ended);
    printf("slots_full_0_max   %" PRIu64 "\n", slots_full_span);
    printf("index_holes        %" PRIu64 "\n", holes);
    printf("fec_sets           %" PRIu64 "\n", (uint64_t)c->fec_n);
    printf("fec_with_params    %" PRIu64 "\n", fec_params);
    printf("fec_data_complete  %" PRIu64 "\n", fec_data_full);
    printf("fec_rs_enough      %" PRIu64 "\n", fec_rs);
    printf("fec_no_data        %" PRIu64 "\n", fec_empty_data);
    if (fec_params != 0) {
        printf("fec_data_complete%% %.3f\n", 100.0 * (double)fec_data_full / (double)fec_params);
        printf("fec_rs_enough%%     %.3f\n", 100.0 * (double)fec_rs / (double)fec_params);
    }
    printf("data_complete_flg  %" PRIu64 "\n", c->data_complete_flags);
    printf("last_in_slot_flg   %" PRIu64 "\n", c->last_in_slot_flags);
    printf("type_merkle_data   %" PRIu64 "\n", c->type_merkle_data);
    printf("type_merkle_code   %" PRIu64 "\n", c->type_merkle_code);
    printf("type_chained_data  %" PRIu64 "\n", c->type_chained_data);
    printf("type_chained_code  %" PRIu64 "\n", c->type_chained_code);
    printf("type_resigned_data %" PRIu64 "\n", c->type_resigned_data);
    printf("type_resigned_code %" PRIu64 "\n", c->type_resigned_code);
    printf("type_legacy_data   %" PRIu64 "\n", c->type_legacy_data);
    printf("type_legacy_code   %" PRIu64 "\n", c->type_legacy_code);
    printf("tsc_hz             %" PRIu64 "\n", c->tsc_hz);
}

int
main(int argc, char **argv)
{
    census_t c;
    const char *dir = NULL;
    int i, any_file = 0;

    memset(&c, 0, sizeof(c));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            any_file = 1;
            if (walk_file(&c, argv[++i]) != 0) {
                return 1;
            }
        } else {
            fprintf(stderr, "usage: %s --dir DIR | --file CAP [...]\n", argv[0]);
            return 1;
        }
    }
    if (dir != NULL) {
        if (walk_dir(&c, dir) != 0) {
            return 1;
        }
    } else if (!any_file) {
        fprintf(stderr, "usage: %s --dir DIR | --file CAP [...]\n", argv[0]);
        return 1;
    }
    report(&c);
    free(c.fec);
    free(c.slot);
    return 0;
}
