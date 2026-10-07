#include "dlmm.h"
#include "dlmm_cache.h"
#include "tsc.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMBO_N        2000u
#define BENCH_WARMUP   2000u
#define BENCH_ITERS    20000u

struct opts {
    uint64_t loops;
    int cpu;
    int do_mlock;
};

static const char *const BUCKET_NAME[] = {
    "0", "1", "2-4", "5-8", "9+"
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
fill_base(dlmm_state_t *s, int32_t active, uint16_t bin_step)
{
    memset(s, 0, sizeof(*s));
    s->active_id = active;
    s->bin_step = bin_step;
    s->now_ts = 1;
    s->parameters.filter_period = 10;
    s->parameters.decay_period = 120;
    s->parameters.reduction_factor = 5000;
    s->parameters.max_volatility_accumulator = 350000;
}

static void
fill_walk_state(dlmm_state_t *s, int32_t active, uint16_t bin_step,
                uint16_t walk, uint8_t swap_for_y)
{
    int32_t id;
    int32_t dir = swap_for_y ? -1 : 1;
    int32_t sink = active + (int32_t)dir * (int32_t)walk;
    uint16_t n = 0;

    fill_base(s, active, bin_step);
    for (id = active - 16; id <= active + 16; id++) {
        uint64_t liq = 1000ull;
        if (walk == 0 && id == active) {
            liq = 1000000000000ull;
        } else if (walk > 0 && id == sink) {
            liq = 1000000000000ull;
        } else if (walk > 0 && id != active
                   && ((dir < 0 && id < active && id > sink)
                       || (dir > 0 && id > active && id < sink))) {
            liq = 1000ull;
        }
        s->bins[n].id = id;
        if (swap_for_y) {
            s->bins[n].amount_y = liq;
            s->reserve_y += liq;
        } else {
            s->bins[n].amount_x = liq;
            s->reserve_x += liq;
        }
        n++;
    }
    s->nbin = n;
}

static uint64_t
amount_for_walk(uint16_t walk)
{
    return (walk == 0) ? 80ull : ((uint64_t)walk * 3000ull + 500ull);
}

static int
quote_agrees_apply(const dlmm_state_t *s, uint64_t amount_in, uint8_t swap_for_y)
{
    dlmm_state_t snap;
    dlmm_state_t after;
    dlmm_swap_ix_t ix;
    dlmm_apply_result_t res;
    dlmm_quote_t q;
    int apply_rc;
    int quote_rc;

    snap = *s;
    ix.amount_in = amount_in;
    ix.min_amount_out = 0;
    ix.swap_for_y = swap_for_y;
    apply_rc = dlmm_apply_swap(s, &ix, &after, &res);
    quote_rc = dlmm_quote_exact_in(s, amount_in, swap_for_y, &q);
    if (memcmp(s, &snap, sizeof(snap)) != 0) {
        fprintf(stderr, "quote mutated S\n");
        return -1;
    }
    if (apply_rc != 0) {
        if (quote_rc == 0 || q.valid) {
            fprintf(stderr, "quote ok but apply failed\n");
            return -1;
        }
        return 0;
    }
    if (quote_rc != 0 || !q.valid) {
        fprintf(stderr, "quote failed but apply ok\n");
        return -1;
    }
    if (q.amount_out != res.amount_out || q.fee != res.fee) {
        fprintf(stderr, "quote/apply mismatch out=%" PRIu64 "/%" PRIu64
                        " fee=%" PRIu64 "/%" PRIu64 "\n",
                q.amount_out, res.amount_out, q.fee, res.fee);
        return -1;
    }
    {
        int32_t delta = after.active_id - s->active_id;
        if (delta < 0) {
            delta = -delta;
        }
        if (q.bins_crossed != (uint16_t)delta) {
            fprintf(stderr, "bins_crossed %u != %d\n", q.bins_crossed, delta);
            return -1;
        }
    }
    return 0;
}

static int
check_isolation(void)
{
    dlmm_state_t s;
    dlmm_state_t snap;
    dlmm_quote_t q;

    fill_walk_state(&s, 0, 100, 0, 1);
    snap = s;
    if (dlmm_quote_exact_in(&s, 1000, 1, &q) != 0 || !q.valid) {
        fprintf(stderr, "isolation quote failed\n");
        return -1;
    }
    if (memcmp(&s, &snap, sizeof(s)) != 0) {
        fprintf(stderr, "isolation mutated S\n");
        return -1;
    }
    if (q.amount_out == 0 || q.bins_crossed != 0) {
        fprintf(stderr, "isolation empty quote\n");
        return -1;
    }
    return 0;
}

static int
check_fail_closed(void)
{
    dlmm_state_t s;
    dlmm_state_t snap;
    dlmm_quote_t q;

    memset(&s, 0, sizeof(s));
    s.active_id = 0;
    s.bin_step = 100;
    s.now_ts = 1;
    s.parameters.filter_period = 10;
    s.parameters.decay_period = 120;
    s.parameters.reduction_factor = 5000;
    s.parameters.max_volatility_accumulator = 350000;
    s.nbin = 1;
    s.bins[0].id = 0;
    s.bins[0].amount_y = 1000;
    s.reserve_y = 1000;
    snap = s;
    if (dlmm_quote_exact_in(&s, 100000000ull, 1, &q) == 0 || q.valid) {
        fprintf(stderr, "missing bin should fail quote\n");
        return -1;
    }
    if (memcmp(&s, &snap, sizeof(s)) != 0) {
        fprintf(stderr, "fail path mutated S\n");
        return -1;
    }
    return 0;
}

static int
check_quote_on_predicted(void)
{
    dlmm_cache_t cache;
    dlmm_state_t s;
    dlmm_state_t s1;
    dlmm_pool_hot_t spec;
    dlmm_swap_ix_t n;
    dlmm_apply_result_t res;
    dlmm_quote_t q;
    uint64_t our_x = 5000;

    fill_walk_state(&s, 0, 100, 0, 1);
    n.amount_in = 1000;
    n.min_amount_out = 0;
    n.swap_for_y = 1;
    dlmm_cache_init(&cache);
    if (dlmm_cache_put(&cache, 3, &s) != 0) {
        return -1;
    }
    {
        dlmm_pool_hot_t canon = cache.pool[3];
        if (dlmm_cache_predict(&cache, 3, &n, &spec, &res) != 0) {
            fprintf(stderr, "predict for quote failed\n");
            return -1;
        }
        if (memcmp(&cache.pool[3], &canon, sizeof(canon)) != 0) {
            fprintf(stderr, "predict wrote canonical\n");
            return -1;
        }
        dlmm_hot_to_state(&spec, &s1);
        if (quote_agrees_apply(&s1, our_x, 1) != 0) {
            fprintf(stderr, "quote(S', x) != apply(S', x)\n");
            return -1;
        }
        if (dlmm_quote_exact_in(&s1, our_x, 1, &q) != 0 || !q.valid) {
            return -1;
        }
        if (memcmp(&cache.pool[3], &canon, sizeof(canon)) != 0) {
            fprintf(stderr, "quote path wrote canonical\n");
            return -1;
        }
    }
    return 0;
}

static void
apply_combo_params(dlmm_state_t *s, uint32_t i)
{
    static const uint16_t bases[] = { 0, 4000, 10000 };
    static const uint32_t vols[] = { 0, 162, 23171 };

    s->parameters.base_factor = bases[(i / 4) % 3];
    s->parameters.variable_fee_control = (i % 5 == 0) ? 0 : 20000;
    s->parameters.protocol_share = (i % 7 == 0) ? 0 : 1000;
    s->parameters.collect_fee_mode = (uint8_t)((i / 3) % 2);
    s->parameters.base_fee_power_factor = (uint8_t)((i / 11) % 2);
    s->v_parameters.volatility_accumulator = vols[i % 3];
    s->v_parameters.volatility_reference = vols[i % 3];
    s->v_parameters.index_reference = s->active_id;
    s->v_parameters.last_update_timestamp = (i % 2) ? 0 : 1;
    s->now_ts = (i % 9 == 0) ? 200 : 1;
}

static uint16_t
pick_walk(uint32_t i)
{
    uint32_t r = i % 100u;
    if (r < 70u) {
        return 0;
    }
    if (r < 85u) {
        return 1;
    }
    if (r < 95u) {
        return (uint16_t)(2u + (i % 3u));
    }
    if (r < 99u) {
        return (uint16_t)(5u + (i % 4u));
    }
    return (uint16_t)(9u + (i % 8u));
}

static int
run_combos(void)
{
    uint32_t i;
    uint32_t agree = 0;
    uint32_t both_fail = 0;

    for (i = 0; i < COMBO_N; i++) {
        dlmm_state_t s;
        uint16_t walk = pick_walk(i);
        uint8_t sfy = (uint8_t)(i & 1u);
        uint64_t ain;
        int32_t active;
        uint16_t step;

        static const int32_t actives[] = { 0, -100, 1596, -2171 };
        static const uint16_t steps[] = { 1, 10, 20, 100 };
        active = actives[i % 4];
        step = steps[(i / 4) % 4];
        fill_walk_state(&s, active, step, walk, sfy);
        apply_combo_params(&s, i);

        ain = amount_for_walk(walk);
        if ((i % 17u) == 0) {
            ain *= 10ull;
        }
        if (quote_agrees_apply(&s, ain, sfy) != 0) {
            fprintf(stderr, "combo %u walk=%u sfy=%u ain=%" PRIu64 "\n",
                    i, walk, sfy, ain);
            return -1;
        }
        {
            dlmm_quote_t q;
            if (dlmm_quote_exact_in(&s, ain, sfy, &q) == 0 && q.valid) {
                agree++;
            } else {
                both_fail++;
            }
        }
    }
    printf("quote == apply\n");
    printf("  combinations     %u\n", COMBO_N);
    printf("  exact agree      %u\n", agree);
    printf("  both fail-closed %u\n", both_fail);
    printf("  S bit-identical  %u / %u\n\n", COMBO_N, COMBO_N);
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
    uint64_t *bucket_cyc[5];
    uint32_t bucket_n[5] = { 0, 0, 0, 0, 0 };
    uint32_t i;
    uint32_t b;

    for (b = 0; b < 5; b++) {
        bucket_cyc[b] = calloc(BENCH_ITERS, sizeof(uint64_t));
        if (bucket_cyc[b] == NULL) {
            return -1;
        }
    }
    for (i = 0; i < 5; i++) {
        dlmm_state_t st;
        dlmm_quote_t q;
        uint16_t target = (i == 0) ? 0 : (i == 1) ? 1 : (i == 2) ? 3 : (i == 3) ? 6 : 12;
        uint64_t ain = amount_for_walk(target);
        uint64_t it;
        uint64_t loop;

        fill_walk_state(&st, 0, 100, target, 1);
        if (dlmm_quote_exact_in(&st, ain, 1, &q) != 0 || !q.valid) {
            fprintf(stderr, "bench setup quote fail bucket %u\n", i);
            return -1;
        }
        for (it = 0; it < BENCH_WARMUP; it++) {
            (void)dlmm_quote_exact_in(&st, ain, 1, &q);
        }
        for (loop = 0; loop < loops; loop++) {
            for (it = 0; it < BENCH_ITERS && bucket_n[i] < BENCH_ITERS; it++) {
                uint64_t t0, t1;
                t0 = rdtscp();
                (void)dlmm_quote_exact_in(&st, ain, 1, &q);
                t1 = rdtscp();
                if (t1 >= t0) {
                    bucket_cyc[i][bucket_n[i]++] = t1 - t0;
                }
            }
        }
    }

    printf("quote cycles by walk\n");
    printf("  %-6s  %8s %8s %8s\n", "bins", "p50", "p99", "p999");
    for (i = 0; i < 5; i++) {
        uint64_t p50, p99, p999;
        if (bucket_n[i] == 0) {
            printf("  %-6s  %8s\n", BUCKET_NAME[i], "-");
            continue;
        }
        qsort(bucket_cyc[i], bucket_n[i], sizeof(uint64_t), cmp_u64);
        p50 = pct_u64(bucket_cyc[i], bucket_n[i], 0.50);
        p99 = pct_u64(bucket_cyc[i], bucket_n[i], 0.99);
        p999 = pct_u64(bucket_cyc[i], bucket_n[i], 0.999);
        printf("  %-6s  %8" PRIu64 " %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " / %" PRIu64 " ns)\n",
               BUCKET_NAME[i], p50, p99, p999,
               tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99), tsc_to_ns(tsc, p999));
    }
    for (b = 0; b < 5; b++) {
        free(bucket_cyc[b]);
    }
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

    fails += check_isolation() != 0;
    fails += check_fail_closed() != 0;
    fails += check_quote_on_predicted() != 0;
    if (fails) {
        printf("CORE-005  FAIL  contracts %d\n", fails);
        return 1;
    }

    printf("CORE-005\n\n");
    printf("  quote isolation          ok  S bit-identical\n");
    printf("  missing bin              ok  FAIL closed\n");
    printf("  quote(S', x)             ok  == apply(S', x)\n\n");

    if (run_combos() != 0) {
        printf("CORE-005  FAIL  combinations\n");
        return 1;
    }
    if (run_bench(&tsc, o.loops) != 0) {
        printf("CORE-005  FAIL  bench\n");
        return 1;
    }
    return 0;
}
