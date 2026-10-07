#include "cycle.h"
#include "dlmm_cache.h"
#include "tsc.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_WARMUP  2000u
#define BENCH_ITERS   20000u

struct opts {
    uint64_t loops;
    int cpu;
    int do_mlock;
};

static void
usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--cpu N] [--mlock] [--loops N]\n", prog);
}

static int
parse_args(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->loops = 1;
    o->cpu = -1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cpu") == 0) {
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
        } else if (strcmp(argv[i], "--loops") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            if (parse_u64(argv[++i], &o->loops) != 0 || o->loops == 0) {
                return -1;
            }
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/* Price 1.0, no fees, deep X and Y at active 0. */
static void
fill_dlmm_flat(dlmm_state_t *s, uint64_t liq)
{
    memset(s, 0, sizeof(*s));
    s->bin_step = 100;
    s->now_ts = 1;
    s->parameters.filter_period = 10;
    s->parameters.decay_period = 120;
    s->parameters.reduction_factor = 5000;
    s->parameters.max_volatility_accumulator = 350000;
    s->nbin = 1;
    s->bins[0].id = 0;
    s->bins[0].amount_x = liq;
    s->bins[0].amount_y = liq;
    s->reserve_x = liq;
    s->reserve_y = liq;
}

static void
fill_pump(pump_state_t *s, uint64_t base, uint64_t quote)
{
    memset(s, 0, sizeof(*s));
    s->reserve_base = base;
    s->reserve_quote = quote;
}

static int
manual_cycle(const dlmm_state_t *d, const pump_state_t *p,
             uint64_t ain, uint8_t dir, uint64_t *mid, uint64_t *final)
{
    dlmm_quote_t dq;
    pump_quote_t pq;

    if (dir == CYCLE_DLMM_THEN_PUMP) {
        if (dlmm_quote_exact_in(d, ain, 0, &dq) != 0 || !dq.valid) {
            return -1;
        }
        *mid = dq.amount_out;
        if (pump_quote_exact_in(p, *mid, PUMP_DIR_BASE_TO_QUOTE, &pq) != 0
            || !pq.valid) {
            return -1;
        }
        *final = pq.amount_out;
        return 0;
    }
    if (dir == CYCLE_PUMP_THEN_DLMM) {
        if (pump_quote_exact_in(p, ain, PUMP_DIR_QUOTE_TO_BASE, &pq) != 0
            || !pq.valid) {
            return -1;
        }
        *mid = pq.amount_out;
        if (dlmm_quote_exact_in(d, *mid, 1, &dq) != 0 || !dq.valid) {
            return -1;
        }
        *final = dq.amount_out;
        return 0;
    }
    return -1;
}

static int
check_hand(void)
{
    dlmm_state_t d;
    pump_state_t p;
    cycle_quote_t q;

    fill_dlmm_flat(&d, 1000000000000ull);
    fill_pump(&p, 1000, 1000);

    /* A: 100 SOL → 100 TOKEN @ 1.0 → sell into 1000/1000 = 90. PnL −10. */
    if (cycle_quote(&d, &p, 100, CYCLE_DLMM_THEN_PUMP, &q) != 0 || !q.valid) {
        fprintf(stderr, "hand A failed\n");
        return -1;
    }
    if (q.amount_out != 90 || q.gross_profit != -10) {
        fprintf(stderr, "hand A out=%" PRIu64 " pnl=%" PRId64 "\n",
                q.amount_out, q.gross_profit);
        return -1;
    }

    /* B: 100 SOL buy → net 99 → 1000*99/1099 = 90 TOKEN → 90 SOL. PnL −10. */
    if (cycle_quote(&d, &p, 100, CYCLE_PUMP_THEN_DLMM, &q) != 0 || !q.valid) {
        fprintf(stderr, "hand B failed\n");
        return -1;
    }
    if (q.amount_out != 90 || q.gross_profit != -10) {
        fprintf(stderr, "hand B out=%" PRIu64 " pnl=%" PRId64 "\n",
                q.amount_out, q.gross_profit);
        return -1;
    }
    return 0;
}

static int
check_positive_pnl(void)
{
    dlmm_state_t d;
    pump_state_t p;
    cycle_quote_t q;

    fill_dlmm_flat(&d, 1000000000000ull);
    fill_pump(&p, 100, 100000);
    if (cycle_quote(&d, &p, 100, CYCLE_DLMM_THEN_PUMP, &q) != 0 || !q.valid) {
        fprintf(stderr, "pos pnl quote failed\n");
        return -1;
    }
    if (q.gross_profit <= 0 || q.amount_out != 50000) {
        fprintf(stderr, "pos pnl %" PRId64 " out=%" PRIu64 "\n",
                q.gross_profit, q.amount_out);
        return -1;
    }
    return 0;
}

static int
check_matches_manual(void)
{
    dlmm_state_t d;
    pump_state_t p;
    dlmm_state_t ds;
    pump_state_t ps;
    cycle_quote_t q;
    uint64_t mid;
    uint64_t final;
    uint8_t dir;

    fill_dlmm_flat(&d, 1000000000000ull);
    fill_pump(&p, 1000, 1000);
    for (dir = 0; dir < 2; dir++) {
        ds = d;
        ps = p;
        if (manual_cycle(&d, &p, 100, dir, &mid, &final) != 0) {
            fprintf(stderr, "manual %u\n", dir);
            return -1;
        }
        if (cycle_quote(&d, &p, 100, dir, &q) != 0 || !q.valid) {
            fprintf(stderr, "cycle %u\n", dir);
            return -1;
        }
        if (q.amount_out != final || q.amount_in != 100
            || q.gross_profit != (int64_t)final - 100
            || q.direction != dir) {
            fprintf(stderr, "cycle != manual dir=%u\n", dir);
            return -1;
        }
        if (memcmp(&d, &ds, sizeof(d)) != 0 || memcmp(&p, &ps, sizeof(p)) != 0) {
            fprintf(stderr, "canonical mutated dir=%u\n", dir);
            return -1;
        }
    }
    return 0;
}

static int
check_fail_closed(void)
{
    dlmm_state_t d;
    pump_state_t p;
    cycle_quote_t q;

    fill_dlmm_flat(&d, 1000000000000ull);
    fill_pump(&p, 1000, 1000);
    p.status = 1;
    if (cycle_quote(&d, &p, 100, CYCLE_DLMM_THEN_PUMP, &q) == 0 || q.valid) {
        fprintf(stderr, "pump fail should kill cycle\n");
        return -1;
    }
    p.status = 0;
    d.nbin = 0;
    d.reserve_x = 0;
    d.reserve_y = 0;
    if (cycle_quote(&d, &p, 100, CYCLE_DLMM_THEN_PUMP, &q) == 0 || q.valid) {
        fprintf(stderr, "dlmm fail should kill cycle\n");
        return -1;
    }
    fill_dlmm_flat(&d, 1000000000000ull);
    if (cycle_quote(&d, &p, 0, CYCLE_PUMP_THEN_DLMM, &q) == 0 || q.valid) {
        fprintf(stderr, "zero in should fail\n");
        return -1;
    }
    if (cycle_quote(&d, &p, 100, 2, &q) == 0 || q.valid) {
        fprintf(stderr, "bad direction should fail\n");
        return -1;
    }
    if (cycle_quote(&d, &p, (uint64_t)INT64_MAX + 1ull, 0, &q) == 0
        || q.valid) {
        fprintf(stderr, "overflow in should fail\n");
        return -1;
    }
    return 0;
}

static int
check_predicted_s(void)
{
    dlmm_cache_t cache;
    dlmm_state_t s;
    dlmm_state_t s1;
    pump_state_t pump;
    dlmm_pool_hot_t spec;
    dlmm_pool_hot_t canon;
    dlmm_swap_ix_t n;
    dlmm_apply_result_t res;
    cycle_quote_t q;
    uint64_t mid;
    uint64_t final;

    fill_dlmm_flat(&s, 1000000000000ull);
    fill_pump(&pump, 1000, 1000);
    n.amount_in = 50;
    n.min_amount_out = 0;
    n.swap_for_y = 1;
    dlmm_cache_init(&cache);
    if (dlmm_cache_put(&cache, 1, &s) != 0) {
        return -1;
    }
    canon = cache.pool[1];
    if (dlmm_cache_predict(&cache, 1, &n, &spec, &res) != 0) {
        fprintf(stderr, "predict for cycle failed\n");
        return -1;
    }
    if (memcmp(&cache.pool[1], &canon, sizeof(canon)) != 0) {
        fprintf(stderr, "predict wrote canonical\n");
        return -1;
    }
    dlmm_hot_to_state(&spec, &s1);
    if (cycle_quote(&s1, &pump, 80, CYCLE_DLMM_THEN_PUMP, &q) != 0
        || !q.valid) {
        fprintf(stderr, "cycle on S' failed\n");
        return -1;
    }
    if (manual_cycle(&s1, &pump, 80, CYCLE_DLMM_THEN_PUMP, &mid, &final) != 0
        || q.amount_out != final) {
        fprintf(stderr, "cycle(S') != quotes\n");
        return -1;
    }
    if (memcmp(&cache.pool[1], &canon, sizeof(canon)) != 0) {
        fprintf(stderr, "cycle wrote canonical DLMM\n");
        return -1;
    }
    return 0;
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t xa = *(const uint64_t *)a;
    uint64_t xb = *(const uint64_t *)b;
    return (xa > xb) - (xa < xb);
}

static uint64_t
pct_u64(uint64_t *v, uint32_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    qsort(v, n, sizeof(v[0]), cmp_u64);
    return v[(uint32_t)(p * (double)(n - 1))];
}

static int
bench_one(const struct tsc_clock *tsc, const char *name,
          const dlmm_state_t *d, const pump_state_t *p,
          uint8_t dir, uint64_t ain, uint64_t loops)
{
    cycle_quote_t q;
    uint64_t *cyc;
    uint32_t n = 0;
    uint64_t it;
    uint64_t loop;
    uint64_t p50, p99;

    cyc = calloc(BENCH_ITERS, sizeof(uint64_t));
    if (cyc == NULL) {
        return -1;
    }
    if (cycle_quote(d, p, ain, dir, &q) != 0 || !q.valid) {
        fprintf(stderr, "bench setup %s\n", name);
        free(cyc);
        return -1;
    }
    for (it = 0; it < BENCH_WARMUP; it++) {
        (void)cycle_quote(d, p, ain, dir, &q);
    }
    for (loop = 0; loop < loops; loop++) {
        for (it = 0; it < BENCH_ITERS && n < BENCH_ITERS; it++) {
            uint64_t t0, t1;
            t0 = rdtscp();
            (void)cycle_quote(d, p, ain, dir, &q);
            t1 = rdtscp();
            if (t1 >= t0) {
                cyc[n++] = t1 - t0;
            }
        }
    }
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    p50 = pct_u64(cyc, n, 0.50);
    p99 = pct_u64(cyc, n, 0.99);
    printf("  %-22s  %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " ns)\n",
           name, p50, p99, tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99));
    free(cyc);
    return 0;
}

static int
run_bench(const struct tsc_clock *tsc, uint64_t loops)
{
    dlmm_state_t d;
    pump_state_t p;
    cycle_quote_t qa;
    cycle_quote_t qb;
    uint64_t *cyc;
    uint32_t n = 0;
    uint64_t it;
    uint64_t loop;
    uint64_t p50, p99;

    fill_dlmm_flat(&d, 1000000000000ull);
    fill_pump(&p, 1000000, 1000000);

    printf("CORE-007\n\n");
    printf("  %-22s  %8s %8s\n", "direction", "p50", "p99");
    if (bench_one(tsc, "DLMM → Pump", &d, &p, CYCLE_DLMM_THEN_PUMP, 10000, loops) != 0) {
        return -1;
    }
    if (bench_one(tsc, "Pump → DLMM", &d, &p, CYCLE_PUMP_THEN_DLMM, 10000, loops) != 0) {
        return -1;
    }

    cyc = calloc(BENCH_ITERS, sizeof(uint64_t));
    if (cyc == NULL) {
        return -1;
    }
    for (it = 0; it < BENCH_WARMUP; it++) {
        (void)cycle_quote(&d, &p, 10000, CYCLE_DLMM_THEN_PUMP, &qa);
        (void)cycle_quote(&d, &p, 10000, CYCLE_PUMP_THEN_DLMM, &qb);
    }
    for (loop = 0; loop < loops; loop++) {
        for (it = 0; it < BENCH_ITERS && n < BENCH_ITERS; it++) {
            uint64_t t0, t1;
            t0 = rdtscp();
            (void)cycle_quote(&d, &p, 10000, CYCLE_DLMM_THEN_PUMP, &qa);
            (void)cycle_quote(&d, &p, 10000, CYCLE_PUMP_THEN_DLMM, &qb);
            t1 = rdtscp();
            if (t1 >= t0) {
                cyc[n++] = t1 - t0;
            }
        }
    }
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    p50 = pct_u64(cyc, n, 0.50);
    p99 = pct_u64(cyc, n, 0.99);
    printf("  %-22s  %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " ns)\n",
           "both directions", p50, p99, tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99));
    free(cyc);
    return 0;
}

int
main(int argc, char **argv)
{
    struct opts o;
    struct tsc_clock tsc;
    int fails = 0;

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

    fails += check_hand() != 0;
    fails += check_positive_pnl() != 0;
    fails += check_matches_manual() != 0;
    fails += check_fail_closed() != 0;
    fails += check_predicted_s() != 0;
    if (fails) {
        printf("CORE-007  FAIL  contracts %d\n", fails);
        return 1;
    }

    printf("CORE-007  contracts\n");
    printf("  hand 100 SOL             ok  A/B → 90  pnl −10\n");
    printf("  positive pnl             ok  100 → 50000\n");
    printf("  == two quotes            ok  mid is second-leg in\n");
    printf("  canonical untouched      ok\n");
    printf("  fail closed              ok  either leg / overflow\n");
    printf("  cycle(S', canonical)     ok  after DLMM predict\n\n");

    if (run_bench(&tsc, o.loops) != 0) {
        printf("CORE-007  FAIL  bench\n");
        return 1;
    }
    return 0;
}
