/*
 * CAP-003 — read-only CORE replay: shred → relevance → frame → pool.
 * Does not write the .cap. Does not change frozen core.
 */
#include "classify.h"
#include "feed.h"
#include "pool.h"
#include "shred.h"
#include "tx.h"
#include "util.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_CAP_BYTES (1u << 20)

typedef struct {
    uint64_t shred;
    uint64_t bad;
    uint64_t rel;
    uint64_t dlmm;
    uint64_t pump;
    uint64_t framed;
    uint64_t framed_dlmm;
    uint64_t framed_pump;
    uint64_t known;
    uint64_t unknown;
    uint64_t miss_scan_hit;
} cov_t;

typedef struct {
    uint8_t  key[32];
    uint8_t  proto;
    uint8_t  used;
    uint64_t n;
} cand_t;

static cand_t *g_cand;
static uint32_t g_cap;
static uint32_t g_n;

static uint32_t
kh(const uint8_t *k)
{
    uint64_t x;
    memcpy(&x, k, 8);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    return (uint32_t)x;
}

static int
cand_add(const uint8_t *key, uint8_t proto)
{
    uint32_t mask, j, d;

    if (g_cap == 0 || g_n * 10u >= g_cap * 7u) {
        uint32_t cap = g_cap == 0 ? 4096 : g_cap * 2;
        cand_t *n = calloc(cap, sizeof(*n));
        uint32_t i;
        if (n == NULL) {
            return -1;
        }
        mask = cap - 1;
        for (i = 0; i < g_cap; i++) {
            if (!g_cand[i].used) {
                continue;
            }
            j = kh(g_cand[i].key) & mask;
            while (n[j].used) {
                j = (j + 1u) & mask;
            }
            n[j] = g_cand[i];
        }
        free(g_cand);
        g_cand = n;
        g_cap = cap;
    }
    mask = g_cap - 1;
    j = kh(key) & mask;
    for (d = 0; d < g_cap; d++) {
        if (!g_cand[j].used) {
            memcpy(g_cand[j].key, key, 32);
            g_cand[j].proto = proto;
            g_cand[j].used = 1;
            g_cand[j].n = 1;
            g_n++;
            return 1;
        }
        if (memcmp(g_cand[j].key, key, 32) == 0) {
            g_cand[j].n++;
            return 0;
        }
        j = (j + 1u) & mask;
    }
    return -1;
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int
walk_file(cov_t *c, pool_table_t *pools, const char *path)
{
    FILE *f;
    feedcap_hdr_t hdr;
    feed_slot_t slot;
    long sz;
    classify_fn clf;

    f = fopen(path, "rb");
    if (f == NULL) {
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
    clf = classify_have_avx2() ? classify_avx2 : classify_scalar;
    fprintf(stderr, "read  %s\n", path);
    for (;;) {
        shred_view_t v;
        uint16_t plen;
        uint32_t rel;
        msg_keys_t mk;
        pool_cand_t cand;
        affected_pool_t aff;
        int rc = feedcap_read_slot(f, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fclose(f);
            return -1;
        }
        if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
            c->bad++;
            continue;
        }
        c->shred++;
        plen = (uint16_t)(slot.len - (uint16_t)(v.payload - slot.data));
        rel = clf(v.payload, plen);
        if (rel == REL_NONE) {
            continue;
        }
        c->rel++;
        if (rel & REL_DLMM) {
            c->dlmm++;
        }
        if (rel & REL_PUMP) {
            c->pump++;
        }
        rc = affected_from_payload(v.payload, plen, pools, &aff);
        if (rc == 0) {
            c->known++;
            continue;
        }
        if (find_prog_id(v.payload, plen, &mk.keys_off, &cand.protocol) == 0
            && msg_keys_from_prog(v.payload, plen, mk.keys_off, &mk) == 0
            && msg_first_dex_pool(v.payload, plen, &mk, &cand) == 0) {
            c->framed++;
            if (cand.protocol == PROTO_DLMM) {
                c->framed_dlmm++;
            } else if (cand.protocol == PROTO_PUMP) {
                c->framed_pump++;
            }
            if (cand.key != NULL) {
                c->unknown++;
                cand_add(cand.key, cand.protocol);
            }
            continue;
        }
        rc = affected_offset_scan(v.payload, plen, pools, &aff);
        if (rc == 0) {
            c->miss_scan_hit++;
            c->known++;
        } else if (rc == 1) {
            c->framed++;
            c->unknown++;
        }
    }
    fclose(f);
    return 0;
}

static int
walk_dir(cov_t *c, pool_table_t *pools, const char *dir)
{
    DIR *d;
    struct dirent *ent;
    char **paths = NULL;
    uint32_t n = 0, cap = 0, i;
    int rc = 0;

    d = opendir(dir);
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
            if (walk_file(c, pools, paths[i]) != 0) {
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
    cov_t c;
    pool_table_t pools;
    const char *dir = NULL;
    const char *pool_path = NULL;
    int i;

    memset(&c, 0, sizeof(c));
    memset(&pools, 0, sizeof(pools));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--pools") == 0 && i + 1 < argc) {
            pool_path = argv[++i];
        } else {
            fprintf(stderr, "usage: %s --dir DIR [--pools ARBPL]\n", argv[0]);
            return 1;
        }
    }
    if (dir == NULL) {
        fprintf(stderr, "usage: %s --dir DIR [--pools ARBPL]\n", argv[0]);
        return 1;
    }
    if (pool_path != NULL) {
        if (pool_table_load(&pools, pool_path) != 0) {
            fprintf(stderr, "pools load failed\n");
            return 1;
        }
    } else if (pool_table_init(&pools, 8) != 0) {
        return 1;
    }
    if (walk_dir(&c, &pools, dir) != 0) {
        return 1;
    }
    printf("CAP-003  CORE REPLAY (coverage, not throughput)\n");
    printf("shred_ok           %" PRIu64 "\n", c.shred);
    printf("relevant           %" PRIu64 "\n", c.rel);
    printf("dlmm_hit           %" PRIu64 "\n", c.dlmm);
    printf("pump_hit           %" PRIu64 "\n", c.pump);
    printf("framed             %" PRIu64 "\n", c.framed);
    printf("framed_dlmm        %" PRIu64 "\n", c.framed_dlmm);
    printf("framed_pump        %" PRIu64 "\n", c.framed_pump);
    printf("known_pool         %" PRIu64 "\n", c.known);
    printf("unknown_pool       %" PRIu64 "\n", c.unknown);
    printf("core002_scan_hit   %" PRIu64 "\n", c.miss_scan_hit);
    printf("unique_candidates  %" PRIu32 "\n", g_n);
    printf("pool_table_n       %" PRIu32 "\n", pools.n);
    pool_table_free(&pools);
    free(g_cand);
    return 0;
}
