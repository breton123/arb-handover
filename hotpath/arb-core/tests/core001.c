#include "arbrx.h"
#include "classify.h"
#include "pool.h"
#include "rx.h"
#include "tsc.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_LOOPS   8ULL
#define DEFAULT_WARMUP  35512ULL
#define SAMPLE_MAX      4000000u

struct opts {
    const char *file;
    const char *pools;
    uint64_t loops;
    uint64_t warmup;
    int cpu;
    int do_mlock;
};

struct cyc_acc {
    uint64_t *v;
    uint32_t n;
    uint32_t cap;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --file shreds.arbrx --pools pools.bin\n"
            "          [--loops N] [--warmup N] [--cpu N] [--mlock]\n",
            prog);
}

static int parse_args(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->loops = DEFAULT_LOOPS;
    o->warmup = DEFAULT_WARMUP;
    o->cpu = -1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            o->file = argv[++i];
        } else if (strcmp(argv[i], "--pools") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            o->pools = argv[++i];
        } else if (strcmp(argv[i], "--loops") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_u64(argv[++i], &o->loops) != 0 || o->loops == 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--warmup") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_u64(argv[++i], &o->warmup) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--cpu") == 0) {
            uint64_t v;
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_u64(argv[++i], &v) != 0) {
                return -1;
            }
            o->cpu = (int)v;
        } else if (strcmp(argv[i], "--mlock") == 0) {
            o->do_mlock = 1;
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    if (o->file == NULL || o->pools == NULL) {
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
    return v[(uint32_t)(p * (double)(n - 1))];
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

static classify_fn pick_classify(void)
{
    if (classify_have_avx2()) {
        return classify_avx2;
    }
    return classify_scalar;
}

static void print_row(const char *name, struct cyc_acc *a, const struct tsc_clock *tsc)
{
    uint64_t p50, p99, p999;

    if (a->n == 0) {
        printf("  %-22s  %8s %8s %8s   (0 samples)\n", name, "-", "-", "-");
        return;
    }
    p50 = pct_u64(a->v, a->n, 0.50);
    p99 = pct_u64(a->v, a->n, 0.99);
    p999 = pct_u64(a->v, a->n, 0.999);
    printf("  %-22s  %8" PRIu64 " %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " / %" PRIu64 " ns)\n",
           name, p50, p99, p999,
           tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99), tsc_to_ns(tsc, p999));
}

int main(int argc, char **argv)
{
    struct opts o;
    struct tsc_clock tsc;
    uint8_t *buf = NULL;
    size_t buflen = 0;
    arbrx_pkt_t *pkts = NULL;
    uint32_t npkts = 0;
    pool_table_t tab;
    classify_fn cls;
    struct cyc_acc hit_cyc;
    struct cyc_acc miss_cyc;
    uint64_t packets = 0;
    uint64_t relevant = 0;
    uint64_t pool_hit = 0;
    uint64_t framed_no_pool = 0;
    uint64_t unframed = 0;
    uint64_t dlmm_hit = 0;
    uint64_t pump_hit = 0;
    uint64_t sink = 0;
    uint64_t loop;
    uint64_t done = 0;
    uint32_t p;

    memset(&tab, 0, sizeof(tab));
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
        return 1;
    }
    if (arbrx_load(o.file, &buf, &buflen) != 0
        || arbrx_index(buf, buflen, &pkts, &npkts) != 0) {
        fprintf(stderr, "failed to load shreds\n");
        return 1;
    }
    if (pool_table_load(&tab, o.pools) != 0) {
        return 1;
    }
    if (cyc_init(&hit_cyc, SAMPLE_MAX) != 0 || cyc_init(&miss_cyc, SAMPLE_MAX) != 0) {
        return 1;
    }
    cls = pick_classify();

    for (loop = 0; loop < o.loops; loop++) {
        for (p = 0; p < npkts; p++) {
            rx_packet_t pkt;
            shred_view_t view;
            uint16_t plen;
            uint32_t rel;
            affected_pool_t ap;
            uint64_t t0, t1;
            int rc;

            pkt.data = pkts[p].data;
            pkt.len = pkts[p].len;
            pkt.queue = 0;
            packets++;
            if (hot_rx(&pkt, &view) != 0) {
                done++;
                continue;
            }
            plen = (uint16_t)(pkt.len - (uint16_t)(view.payload - pkt.data));
            rel = cls(view.payload, plen);
            if (rel == REL_NONE) {
                done++;
                continue;
            }
            relevant++;

            t0 = rdtscp();
            rc = affected_from_payload(view.payload, plen, &tab, &ap);
            t1 = rdtscp();

            if (rc == 0) {
                pool_hit++;
                sink += (uint64_t)ap.pool_idx + (uint64_t)ap.protocol;
                if (ap.protocol == PROTO_DLMM) {
                    dlmm_hit++;
                } else if (ap.protocol == PROTO_PUMP) {
                    pump_hit++;
                }
                if (done >= o.warmup && t1 >= t0) {
                    cyc_push(&hit_cyc, t1 - t0);
                }
            } else if (rc == 1) {
                framed_no_pool++;
                if (done >= o.warmup && t1 >= t0) {
                    cyc_push(&miss_cyc, t1 - t0);
                }
            } else {
                unframed++;
                if (done >= o.warmup && t1 >= t0) {
                    cyc_push(&miss_cyc, t1 - t0);
                }
            }
            done++;
        }
    }

    printf("CORE-001\n\n");
    printf("packets          %" PRIu64 "\n", packets);
    printf("relevant         %" PRIu64 "\n", relevant);
    printf("  pool hit       %" PRIu64 "\n", pool_hit);
    printf("  framed no pool %" PRIu64 "\n", framed_no_pool);
    printf("  unframed       %" PRIu64 "\n", unframed);
    printf("  DLMM hit       %" PRIu64 "\n", dlmm_hit);
    printf("  Pump hit       %" PRIu64 "\n", pump_hit);
    printf("known pools      %u\n\n", tab.n);
    printf("cycles:                    p50      p99     p999\n");
    print_row("relevant → pool", &hit_cyc, &tsc);
    print_row("relevant → no pool", &miss_cyc, &tsc);
    printf("\nsink %" PRIu64 "\n", sink);

    free(hit_cyc.v);
    free(miss_cyc.v);
    pool_table_free(&tab);
    free(pkts);
    free(buf);
    return 0;
}
