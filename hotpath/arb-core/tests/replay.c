#include "classify.h"
#include "rx.h"
#include "tsc.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARBRX_MAGIC        "ARBRX1"
#define ARBRX_MAGIC_LEN    8
#define DEFAULT_LOOPS      8ULL
#define DEFAULT_WARMUP     35512ULL
#define SAMPLE_MAX         4000000u

enum scanner_id {
    SCAN_SCALAR = 0,
    SCAN_AVX2,
    SCAN_AVX512,
    SCAN_ALL,
    SCAN_N
};

static const char *scanner_name[] = {
    "scalar",
    "AVX2",
    "AVX-512",
};

struct opts {
    const char *file;
    uint64_t loops;
    uint64_t warmup;
    int cpu;
    int do_mlock;
    enum scanner_id scanner;
};

struct packet_ref {
    const uint8_t *data;
    uint16_t len;
};

struct cyc_acc {
    uint64_t *v;
    uint32_t n;
    uint32_t cap;
};

struct counts {
    uint64_t packets;
    uint64_t parsed;
    uint64_t bad;
    uint64_t dlmm;
    uint64_t pump;
    uint64_t both;
    uint64_t irrelevant;
    uint64_t relevant;
    uint64_t sink;
};

struct run {
    const char *name;
    classify_fn fn;
    struct counts c;
    struct cyc_acc parse;
    struct cyc_acc classify;
    struct cyc_acc classify_irrel;
    struct cyc_acc classify_rel;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --file shreds.arbrx [--scanner all|scalar|avx2|avx512]\n"
            "          [--loops N] [--warmup N] [--cpu N] [--mlock]\n",
            prog);
}

static int parse_scanner(const char *s, enum scanner_id *out)
{
    if (strcmp(s, "all") == 0) {
        *out = SCAN_ALL;
        return 0;
    }
    if (strcmp(s, "scalar") == 0) {
        *out = SCAN_SCALAR;
        return 0;
    }
    if (strcmp(s, "avx2") == 0) {
        *out = SCAN_AVX2;
        return 0;
    }
    if (strcmp(s, "avx512") == 0) {
        *out = SCAN_AVX512;
        return 0;
    }
    return -1;
}

static int parse_args(int argc, char **argv, struct opts *o)
{
    int i;

    o->file = NULL;
    o->loops = DEFAULT_LOOPS;
    o->warmup = DEFAULT_WARMUP;
    o->cpu = -1;
    o->do_mlock = 0;
    o->scanner = SCAN_ALL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            o->file = argv[++i];
        } else if (strcmp(argv[i], "--scanner") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_scanner(argv[++i], &o->scanner) != 0) {
                fprintf(stderr, "bad --scanner\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--loops") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_u64(argv[++i], &o->loops) != 0 || o->loops == 0) {
                fprintf(stderr, "bad --loops\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--warmup") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_u64(argv[++i], &o->warmup) != 0) {
                fprintf(stderr, "bad --warmup\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--cpu") == 0) {
            uint64_t v;
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_u64(argv[++i], &v) != 0 || v > 4096) {
                fprintf(stderr, "bad --cpu\n");
                return -1;
            }
            o->cpu = (int)v;
        } else if (strcmp(argv[i], "--mlock") == 0) {
            o->do_mlock = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return -1;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    if (o->file == NULL) {
        usage(argv[0]);
        return -1;
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t xa = *(const uint64_t *)a;
    uint64_t xb = *(const uint64_t *)b;
    return (xa > xb) - (xa < xb);
}

static uint64_t pct_u64(uint64_t *v, uint32_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    qsort(v, n, sizeof(v[0]), cmp_u64);
    {
        uint32_t i = (uint32_t)(p * (double)(n - 1));
        return v[i];
    }
}

static int cyc_init(struct cyc_acc *a, uint32_t cap)
{
    a->v = malloc((size_t)cap * sizeof(*a->v));
    a->n = 0;
    a->cap = cap;
    return a->v == NULL ? -1 : 0;
}

static void cyc_push(struct cyc_acc *a, uint64_t c)
{
    if (a->n < a->cap) {
        a->v[a->n++] = c;
    }
}

static int load_file(const char *path, uint8_t **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t n;
    long sz;

    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < ARBRX_MAGIC_LEN) {
        fprintf(stderr, "%s: too short\n", path);
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        perror("fseek");
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)sz);
    if (buf == NULL) {
        fprintf(stderr, "oom\n");
        fclose(f);
        return -1;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        return -1;
    }
    if (memcmp(buf, ARBRX_MAGIC, 6) != 0) {
        fprintf(stderr, "%s: not ARBRX1\n", path);
        free(buf);
        return -1;
    }
    *out_buf = buf;
    *out_len = n;
    return 0;
}

static int index_packets(const uint8_t *buf, size_t len,
                         struct packet_ref **out_pkts, uint32_t *out_n)
{
    size_t off = ARBRX_MAGIC_LEN;
    uint32_t cap = 1024;
    uint32_t n = 0;
    struct packet_ref *pkts = malloc((size_t)cap * sizeof(*pkts));

    if (pkts == NULL) {
        fprintf(stderr, "oom\n");
        return -1;
    }
    while (off + 2 <= len) {
        uint16_t plen = shred_load_u16_le(buf + off);
        off += 2;
        if (plen == 0 || off + plen > len) {
            fprintf(stderr, "truncated record at %zu (len=%u)\n", off, plen);
            free(pkts);
            return -1;
        }
        if (n == cap) {
            uint32_t ncap = cap * 2u;
            struct packet_ref *np = realloc(pkts, (size_t)ncap * sizeof(*pkts));
            if (np == NULL) {
                fprintf(stderr, "oom\n");
                free(pkts);
                return -1;
            }
            pkts = np;
            cap = ncap;
        }
        pkts[n].data = buf + off;
        pkts[n].len = plen;
        n++;
        off += plen;
    }
    if (n == 0) {
        fprintf(stderr, "no packets\n");
        free(pkts);
        return -1;
    }
    *out_pkts = pkts;
    *out_n = n;
    return 0;
}

static void replay(const struct packet_ref *pkts, uint32_t npkts,
                   uint64_t loops, uint64_t warmup, struct run *r)
{
    uint64_t i, done = 0;
    uint32_t p;

    for (i = 0; i < loops; i++) {
        for (p = 0; p < npkts; p++) {
            rx_packet_t pkt;
            shred_view_t view;
            uint64_t t0, t1, t2;
            uint32_t hit;
            uint16_t plen;
            int ok;

            pkt.data = pkts[p].data;
            pkt.len = pkts[p].len;
            pkt.queue = 0;

            t0 = rdtscp();
            ok = hot_rx(&pkt, &view);
            t1 = rdtscp();

            r->c.packets++;
            if (ok != 0) {
                r->c.bad++;
                done++;
                continue;
            }
            r->c.parsed++;

            plen = (uint16_t)(pkt.len - (uint16_t)(view.payload - pkt.data));
            hit = r->fn(view.payload, plen);
            t2 = rdtscp();

            if (hit & REL_DLMM) {
                r->c.dlmm++;
            }
            if (hit & REL_PUMP) {
                r->c.pump++;
            }
            if (hit == REL_BOTH) {
                r->c.both++;
            }
            if (hit == REL_NONE) {
                r->c.irrelevant++;
            } else {
                r->c.relevant++;
            }
            r->c.sink += view.slot + (uint64_t)hit + (uintptr_t)view.payload;

            if (done >= warmup && t1 >= t0 && t2 >= t1) {
                cyc_push(&r->parse, t1 - t0);
                cyc_push(&r->classify, t2 - t1);
                if (hit == REL_NONE) {
                    cyc_push(&r->classify_irrel, t2 - t1);
                } else {
                    cyc_push(&r->classify_rel, t2 - t1);
                }
            }
            done++;
        }
    }
}

static int counts_equal(const struct counts *a, const struct counts *b)
{
    return a->packets == b->packets && a->parsed == b->parsed
        && a->bad == b->bad && a->dlmm == b->dlmm && a->pump == b->pump
        && a->both == b->both && a->irrelevant == b->irrelevant;
}

static int run_init(struct run *r, const char *name, classify_fn fn)
{
    r->name = name;
    r->fn = fn;
    memset(&r->c, 0, sizeof(r->c));
    if (cyc_init(&r->parse, SAMPLE_MAX) != 0
        || cyc_init(&r->classify, SAMPLE_MAX) != 0
        || cyc_init(&r->classify_irrel, SAMPLE_MAX) != 0
        || cyc_init(&r->classify_rel, SAMPLE_MAX) != 0) {
        return -1;
    }
    return 0;
}

static void run_free(struct run *r)
{
    free(r->parse.v);
    free(r->classify.v);
    free(r->classify_irrel.v);
    free(r->classify_rel.v);
}

static void print_counts(const struct counts *c)
{
    printf("packets       %" PRIu64 "\n", c->packets);
    printf("parsed        %" PRIu64 "\n", c->parsed);
    printf("bad           %" PRIu64 "\n", c->bad);
    printf("relevant\n");
    printf("  DLMM        %" PRIu64 "\n", c->dlmm);
    printf("  Pump        %" PRIu64 "\n", c->pump);
    printf("  both        %" PRIu64 "\n", c->both);
    printf("irrelevant    %" PRIu64 "\n", c->irrelevant);
}

static void print_table_row(const char *name, struct run *r, const struct tsc_clock *tsc)
{
    uint64_t all50, ir50, p99, p999, rel50;

    if (r->classify.n == 0) {
        printf("  %-10s  %8s %8s %8s %8s\n", name, "-", "-", "-", "-");
        return;
    }
    all50 = pct_u64(r->classify.v, r->classify.n, 0.50);
    ir50 = r->classify_irrel.n ? pct_u64(r->classify_irrel.v, r->classify_irrel.n, 0.50) : 0;
    p99 = pct_u64(r->classify_irrel.n ? r->classify_irrel.v : r->classify.v,
                  r->classify_irrel.n ? r->classify_irrel.n : r->classify.n, 0.99);
    p999 = pct_u64(r->classify_irrel.n ? r->classify_irrel.v : r->classify.v,
                   r->classify_irrel.n ? r->classify_irrel.n : r->classify.n, 0.999);
    rel50 = r->classify_rel.n ? pct_u64(r->classify_rel.v, r->classify_rel.n, 0.50) : 0;
    printf("  %-10s  %8" PRIu64 " %8" PRIu64 " %8" PRIu64 " %8" PRIu64
           "    rel p50 %" PRIu64 "  (%" PRIu64 " / %" PRIu64 " ns irrel)\n",
           name, all50, ir50, p99, p999, rel50,
           tsc_to_ns(tsc, ir50), tsc_to_ns(tsc, p99));
}

int main(int argc, char **argv)
{
    struct opts o;
    struct tsc_clock tsc;
    uint8_t *buf = NULL;
    size_t buflen = 0;
    struct packet_ref *pkts = NULL;
    uint32_t npkts = 0;
    struct run runs[3];
    int nrun = 0;
    int i;
    int mismatch = 0;
    classify_fn fns[3] = { classify_scalar, classify_avx2, classify_avx512 };

    if (parse_args(argc, argv, &o) != 0) {
        return 1;
    }
    if (pin_cpu(o.cpu) != 0) {
        return 1;
    }
    if (o.do_mlock && lock_memory() != 0) {
        return 1;
    }
    if (tsc_calibrate(&tsc) != 0) {
        fprintf(stderr, "tsc calibrate failed\n");
        return 1;
    }
    if (load_file(o.file, &buf, &buflen) != 0) {
        return 1;
    }
    if (index_packets(buf, buflen, &pkts, &npkts) != 0) {
        free(buf);
        return 1;
    }

    for (i = 0; i < 3; i++) {
        int want;

        if (o.scanner == SCAN_ALL) {
            want = 1;
        } else {
            want = (i == (int)o.scanner);
        }
        if (!want) {
            continue;
        }
        if (i == SCAN_AVX2 && !classify_have_avx2()) {
            fprintf(stderr, "AVX2 not available\n");
            if (o.scanner == SCAN_AVX2) {
                free(pkts);
                free(buf);
                return 1;
            }
            continue;
        }
        if (i == SCAN_AVX512 && !classify_have_avx512()) {
            fprintf(stderr, "AVX-512 not available\n");
            if (o.scanner == SCAN_AVX512) {
                free(pkts);
                free(buf);
                return 1;
            }
            continue;
        }
        if (run_init(&runs[nrun], scanner_name[i], fns[i]) != 0) {
            fprintf(stderr, "oom\n");
            free(pkts);
            free(buf);
            return 1;
        }
        replay(pkts, npkts, o.loops, o.warmup, &runs[nrun]);
        nrun++;
    }

    printf("REPLAY  recorded %u  loops %" PRIu64 "\n\n", npkts, o.loops);
    if (nrun > 0) {
        print_counts(&runs[0].c);
    }
    for (i = 1; i < nrun; i++) {
        if (!counts_equal(&runs[0].c, &runs[i].c)) {
            printf("\nCOUNT MISMATCH %s vs %s — SIMD loses\n",
                   runs[i].name, runs[0].name);
            print_counts(&runs[i].c);
            mismatch = 1;
        }
    }
    if (nrun > 1 && !mismatch) {
        printf("\ncounts bit-identical across %d scanners\n", nrun);
    }

    printf("\nScanner       all p50  irrel p50      p99     p999\n");
    for (i = 0; i < nrun; i++) {
        print_table_row(runs[i].name, &runs[i], &tsc);
    }
    if (nrun > 0) {
        printf("\nshred parse p50 %" PRIu64 " cyc  sink %" PRIu64 "\n",
               runs[0].parse.n ? pct_u64(runs[0].parse.v, runs[0].parse.n, 0.50) : 0,
               runs[0].c.sink);
    }

    for (i = 0; i < nrun; i++) {
        run_free(&runs[i]);
    }
    free(pkts);
    free(buf);
    return mismatch ? 2 : 0;
}
