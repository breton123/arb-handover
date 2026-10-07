#include "cycle.h"
#include "tsc.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_WARMUP  500u
#define BENCH_ITERS   2000u
#define SOL           1000000000ull

static const uint64_t LADDER[] = {
    SOL / 100ull, SOL / 50ull, SOL / 20ull, SOL / 10ull, SOL / 5ull,
    SOL / 2ull, SOL, SOL * 2ull, SOL * 5ull, SOL * 10ull,
    SOL * 20ull, SOL * 50ull, SOL * 100ull
};
#define LADDER_N  (sizeof(LADDER) / sizeof(LADDER[0]))

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
fill_pump(pump_state_t *s, uint64_t base, uint64_t quote,
          uint64_t lp, uint64_t proto)
{
    memset(s, 0, sizeof(*s));
    s->reserve_base = base;
    s->reserve_quote = quote;
    s->lp_fee_bps = lp;
    s->protocol_fee_bps = proto;
}

static int
check_no_edge(void)
{
    dlmm_state_t d;
    pump_state_t p;
    dlmm_state_t ds;
    pump_state_t ps;
    opportunity_t o;

    fill_dlmm_flat(&d, 1000000000000000ull);
    fill_pump(&p, 1000000000000ull, 1000000000000ull, 20, 5);
    ds = d;
    ps = p;
    if (cycle_size(&d, &p, &o) != 0) {
        fprintf(stderr, "flat size failed\n");
        return -1;
    }
    if (o.valid) {
        fprintf(stderr, "1:1+fees should have no +pnl\n");
        return -1;
    }
    if (memcmp(&d, &ds, sizeof(d)) != 0 || memcmp(&p, &ps, sizeof(p)) != 0) {
        fprintf(stderr, "size mutated\n");
        return -1;
    }
    return 0;
}

static int
check_picks_best(void)
{
    dlmm_state_t d;
    pump_state_t p;
    opportunity_t o;
    cycle_quote_t q;
    uint32_t i;
    uint8_t dir;
    int64_t coarse_best = 0;

    fill_dlmm_flat(&d, 1000000000000000ull);
    /* Cheap TOKEN on DLMM (1:1), rich TOKEN on Pump → A is +pnl. */
    fill_pump(&p, 20000000000ull, 2000000000000ull, 0, 0);
    if (cycle_size(&d, &p, &o) != 0 || !o.valid) {
        fprintf(stderr, "expected an opportunity\n");
        return -1;
    }
    if (o.route_id != ROUTE_DLMM_PUMP || o.gross_profit == 0) {
        fprintf(stderr, "empty opportunity\n");
        return -1;
    }
    if (cycle_quote(&d, &p, o.amount_in, o.direction, &q) != 0 || !q.valid) {
        fprintf(stderr, "replay quote failed\n");
        return -1;
    }
    if (q.amount_out != o.amount_out
        || (uint64_t)q.gross_profit != o.gross_profit) {
        fprintf(stderr, "size != cycle_quote\n");
        return -1;
    }
    for (dir = 0; dir < 2; dir++) {
        for (i = 0; i < LADDER_N; i++) {
            if (cycle_quote(&d, &p, LADDER[i], dir, &q) != 0 || !q.valid) {
                continue;
            }
            if (q.gross_profit > coarse_best) {
                coarse_best = q.gross_profit;
            }
        }
    }
    if ((int64_t)o.gross_profit < coarse_best) {
        fprintf(stderr, "refine worse than coarse %" PRIu64 " < %" PRId64 "\n",
                o.gross_profit, coarse_best);
        return -1;
    }
    return 0;
}

static int
check_fail_closed(void)
{
    dlmm_state_t d;
    pump_state_t p;
    opportunity_t o;

    memset(&d, 0, sizeof(d));
    memset(&p, 0, sizeof(p));
    if (cycle_size(&d, &p, &o) != 0) {
        fprintf(stderr, "empty pools should return 0\n");
        return -1;
    }
    if (o.valid) {
        fprintf(stderr, "empty pools not an opportunity\n");
        return -1;
    }
    if (cycle_size(NULL, &p, &o) == 0 && o.valid) {
        fprintf(stderr, "null should fail\n");
        return -1;
    }
    return 0;
}

static int
check_deterministic(void)
{
    dlmm_state_t d;
    pump_state_t p;
    opportunity_t a;
    opportunity_t b;

    fill_dlmm_flat(&d, 1000000000000000ull);
    fill_pump(&p, 20000000000ull, 2000000000000ull, 0, 0);
    if (cycle_size(&d, &p, &a) != 0 || cycle_size(&d, &p, &b) != 0) {
        return -1;
    }
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        fprintf(stderr, "size not deterministic\n");
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
run_bench(const struct tsc_clock *tsc, uint64_t loops)
{
    dlmm_state_t d;
    pump_state_t p;
    opportunity_t o;
    uint64_t *cyc;
    uint32_t n = 0;
    uint64_t it;
    uint64_t loop;
    uint64_t p50, p99;

    fill_dlmm_flat(&d, 1000000000000000ull);
    fill_pump(&p, 20000000000ull, 2000000000000ull, 0, 0);
    cyc = calloc(BENCH_ITERS, sizeof(uint64_t));
    if (cyc == NULL) {
        return -1;
    }
    if (cycle_size(&d, &p, &o) != 0 || !o.valid) {
        fprintf(stderr, "bench setup\n");
        free(cyc);
        return -1;
    }
    for (it = 0; it < BENCH_WARMUP; it++) {
        (void)cycle_size(&d, &p, &o);
    }
    for (loop = 0; loop < loops; loop++) {
        for (it = 0; it < BENCH_ITERS && n < BENCH_ITERS; it++) {
            uint64_t t0, t1;
            t0 = rdtscp();
            (void)cycle_size(&d, &p, &o);
            t1 = rdtscp();
            if (t1 >= t0) {
                cyc[n++] = t1 - t0;
            }
        }
    }
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    p50 = pct_u64(cyc, n, 0.50);
    p99 = pct_u64(cyc, n, 0.99);
    printf("CORE-008\n\n");
    printf("  size                 p50      p99\n");
    printf("  cycle_size       %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " ns)\n",
           p50, p99, tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99));
    printf("  picked               %" PRIu64 " lamports  dir %u  pnl %" PRIu64 "\n",
           o.amount_in, o.direction, o.gross_profit);
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

    fails += check_no_edge() != 0;
    fails += check_picks_best() != 0;
    fails += check_fail_closed() != 0;
    fails += check_deterministic() != 0;
    if (fails) {
        printf("CORE-008  FAIL  contracts %d\n", fails);
        return 1;
    }

    printf("CORE-008  contracts\n");
    printf("  no +pnl                  ok  valid=0  S untouched\n");
    printf("  argmax >= coarse         ok  == cycle_quote\n");
    printf("  fail closed              ok  empty / null\n");
    printf("  deterministic            ok\n\n");

    if (run_bench(&tsc, o.loops) != 0) {
        printf("CORE-008  FAIL  bench\n");
        return 1;
    }
    return 0;
}
