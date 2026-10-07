#include "dlmm.h"
#include "dlmm_cache.h"
#include "tsc.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYNTH_N        2000u
#define BENCH_WARMUP   2000u
#define BENCH_ITERS    20000u

struct opts {
    const char *cap;
    uint64_t loops;
    int cpu;
    int do_mlock;
};

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--cap FILE] [--cpu N] [--mlock] [--loops N]\n",
            prog);
}

static int
parse_args(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->loops = 1;
    o->cpu = -1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cap") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            o->cap = argv[++i];
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
fill_base(dlmm_state_t *s, int32_t active)
{
    memset(s, 0, sizeof(*s));
    s->active_id = active;
    s->bin_step = 100;
    s->now_ts = 1;
    s->parameters.filter_period = 10;
    s->parameters.decay_period = 120;
    s->parameters.reduction_factor = 5000;
    s->parameters.max_volatility_accumulator = 350000;
}

static void
fill_window_state(dlmm_state_t *s, int32_t active, uint64_t edge_y, uint64_t sink_y)
{
    int32_t id;
    uint16_t n = 0;

    fill_base(s, active);
    for (id = active - (int32_t)DLMM_CACHE_K; id <= active + (int32_t)DLMM_CACHE_K; id++) {
        s->bins[n].id = id;
        s->bins[n].amount_y = (id == active - (int32_t)DLMM_CACHE_K) ? sink_y : edge_y;
        s->reserve_y += s->bins[n].amount_y;
        n++;
    }
    s->nbin = n;
}

/* Window live. Drain bins on the walk path; sink sits at active ± walk. */
static void
fill_walk_state(dlmm_state_t *s, uint16_t walk, uint8_t swap_for_y)
{
    int32_t id;
    int32_t dir = swap_for_y ? -1 : 1;
    int32_t sink = (int32_t)dir * (int32_t)walk;
    uint16_t n = 0;

    fill_base(s, 0);
    for (id = -(int32_t)DLMM_CACHE_K; id <= (int32_t)DLMM_CACHE_K; id++) {
        uint64_t liq = 1000ull;
        if (walk == 0 && id == 0) {
            liq = 1000000000000ull;
        } else if (walk > 0 && id == sink) {
            liq = 1000000000000ull;
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

static int
check_isolation(void)
{
    dlmm_cache_t cache;
    dlmm_state_t s;
    dlmm_pool_hot_t snap;
    dlmm_pool_hot_t spec;
    dlmm_swap_ix_t ix = { .amount_in = 1000, .min_amount_out = 0, .swap_for_y = 1 };
    dlmm_apply_result_t res;

    fill_window_state(&s, 0, 1000000ull, 1000000ull);
    dlmm_cache_init(&cache);
    if (dlmm_cache_put(&cache, 7, &s) != 0) {
        fprintf(stderr, "put failed\n");
        return -1;
    }
    snap = cache.pool[7];
    if (dlmm_cache_predict(&cache, 7, &ix, &spec, &res) != 0) {
        fprintf(stderr, "predict failed\n");
        return -1;
    }
    if (memcmp(&cache.pool[7], &snap, sizeof(snap)) != 0) {
        fprintf(stderr, "canonical mutated\n");
        return -1;
    }
    if (spec.reserve_y == snap.reserve_y || spec.bin[DLMM_CACHE_K].amount_x == 0) {
        fprintf(stderr, "speculative copy did not change\n");
        return -1;
    }
    if (dlmm_cache_commit(&cache, 7, &spec) != 0
        || memcmp(&cache.pool[7], &spec, sizeof(spec)) != 0) {
        fprintf(stderr, "commit failed\n");
        return -1;
    }
    return 0;
}

static int
check_missing_bin(void)
{
    dlmm_cache_t cache;
    dlmm_state_t s;
    dlmm_pool_hot_t spec;
    dlmm_swap_ix_t ix = { .amount_in = 100000000ull, .min_amount_out = 0, .swap_for_y = 1 };
    dlmm_apply_result_t res;

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
    dlmm_cache_init(&cache);
    if (dlmm_cache_put(&cache, 0, &s) != 0) {
        return -1;
    }
    if (dlmm_cache_predict(&cache, 0, &ix, &spec, &res) == 0) {
        fprintf(stderr, "missing bin should fail\n");
        return -1;
    }
    if (!cache.pool[0].occupied || cache.pool[0].reserve_y != 1000) {
        fprintf(stderr, "fail path mutated canonical\n");
        return -1;
    }
    return 0;
}

static void
fill_id_range(dlmm_state_t *s, int32_t lo, int32_t hi, uint64_t y_edge, int32_t sink_id, uint64_t y_sink)
{
    int32_t id;
    uint16_t n = 0;

    memset(s, 0, sizeof(*s));
    s->active_id = 0;
    s->bin_step = 100;
    s->now_ts = 1;
    s->parameters.filter_period = 10;
    s->parameters.decay_period = 120;
    s->parameters.reduction_factor = 5000;
    s->parameters.max_volatility_accumulator = 350000;
    for (id = lo; id <= hi; id++) {
        s->bins[n].id = id;
        s->bins[n].amount_y = (id == sink_id) ? y_sink : y_edge;
        s->reserve_y += s->bins[n].amount_y;
        n++;
    }
    s->nbin = n;
}

static int
check_window_edge(void)
{
    dlmm_cache_t cache;
    dlmm_state_t s;
    dlmm_pool_hot_t spec;
    dlmm_apply_result_t res;
    dlmm_swap_ix_t ix = { .amount_in = 200000000ull, .min_amount_out = 0, .swap_for_y = 1 };

    fill_id_range(&s, -20, 20, 1000ull, -16, 1000000000000ull);
    dlmm_cache_init(&cache);
    if (dlmm_cache_put(&cache, 1, &s) != 0) {
        return -1;
    }
    if (dlmm_hot_slot(&cache.pool[1], -16) < 0 || dlmm_hot_slot(&cache.pool[1], -17) >= 0) {
        fprintf(stderr, "window bounds\n");
        return -1;
    }
    if (dlmm_cache_predict(&cache, 1, &ix, &spec, &res) != 0) {
        fprintf(stderr, "walk 16 should fit ±K\n");
        return -1;
    }
    if (spec.active_id != -16) {
        fprintf(stderr, "walk 16 landed %d\n", spec.active_id);
        return -1;
    }

    fill_id_range(&s, -20, 20, 1000ull, -99, 1000ull);
    if (dlmm_cache_put(&cache, 1, &s) != 0) {
        return -1;
    }
    if (dlmm_cache_predict(&cache, 1, &ix, &spec, &res) == 0) {
        fprintf(stderr, "walk 17 should miss cold bin\n");
        return -1;
    }
    return 0;
}

static int
check_direct_index(void)
{
    dlmm_cache_t cache;
    dlmm_state_t s;
    const dlmm_pool_hot_t *h;

    fill_window_state(&s, 5, 1, 1);
    dlmm_cache_init(&cache);
    if (dlmm_cache_put(&cache, 37, &s) != 0) {
        return -1;
    }
    h = dlmm_cache_get(&cache, 37);
    if (h == NULL || h != &cache.pool[37] || h->active_id != 5) {
        fprintf(stderr, "direct index\n");
        return -1;
    }
    if (dlmm_cache_get(&cache, 36) != NULL) {
        fprintf(stderr, "empty slot should miss\n");
        return -1;
    }
    return 0;
}

static void
state_to_cap_bins(const dlmm_state_t *s, dlmm_hot_bin_t *bins, uint16_t *n)
{
    uint16_t i;
    uint16_t m = s->nbin;

    if (m > DLMM_CAP_BINS) {
        m = DLMM_CAP_BINS;
    }
    for (i = 0; i < m; i++) {
        bins[i].id = s->bins[i].id;
        bins[i].amount_x = s->bins[i].amount_x;
        bins[i].amount_y = s->bins[i].amount_y;
    }
    *n = m;
}

static void
make_synth_rec(dlmm_cap_rec_t *rec, uint32_t pool_idx, uint16_t walk, uint8_t swap_for_y)
{
    dlmm_state_t before;
    dlmm_state_t after;
    dlmm_swap_ix_t ix;
    dlmm_apply_result_t res;
    int32_t delta;

    memset(rec, 0, sizeof(*rec));
    fill_walk_state(&before, walk, swap_for_y);
    ix.min_amount_out = 0;
    ix.swap_for_y = swap_for_y;
    ix.amount_in = (walk == 0) ? 80ull : ((uint64_t)walk * 3000ull + 500ull);
    if (dlmm_apply_swap(&before, &ix, &after, &res) != 0) {
        ix.amount_in = 50;
        if (dlmm_apply_swap(&before, &ix, &after, &res) != 0) {
            memcpy(&after, &before, sizeof(after));
            res.active_id_after = before.active_id;
        }
    }
    rec->pool_idx = pool_idx;
    rec->swap_for_y = swap_for_y;
    rec->amount_in = ix.amount_in;
    rec->min_out = 0;
    rec->now_ts = before.now_ts;
    rec->before_active = before.active_id;
    rec->after_active = after.active_id;
    rec->bin_step = before.bin_step;
    rec->status = 0;
    rec->parameters = before.parameters;
    rec->v_before = before.v_parameters;
    rec->v_after = after.v_parameters;
    rec->before_rx = before.reserve_x;
    rec->before_ry = before.reserve_y;
    rec->after_rx = after.reserve_x;
    rec->after_ry = after.reserve_y;
    state_to_cap_bins(&before, rec->bins_before, &rec->nbin_before);
    state_to_cap_bins(&after, rec->bins_after, &rec->nbin_after);
    delta = rec->after_active - rec->before_active;
    if (delta < 0) {
        delta = -delta;
    }
    rec->crossed = (uint8_t)delta;
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
cmp_u32(const void *a, const void *b)
{
    uint32_t xa = *(const uint32_t *)a;
    uint32_t xb = *(const uint32_t *)b;
    return (xa > xb) - (xa < xb);
}

static uint32_t
pct_u32(uint32_t *v, uint32_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    return v[(uint32_t)(p * (double)(n - 1))];
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

static const char *const BUCKET_NAME[] = {
    "0", "1", "2-4", "5-8", "9+"
};

static int
run_synth(const struct tsc_clock *tsc, uint64_t loops)
{
    dlmm_cap_rec_t *recs;
    uint32_t i;
    uint32_t match = 0;
    uint32_t insuff = 0;
    uint32_t mismatch = 0;
    uint32_t *crossed;
    uint32_t *bytes;
    uint32_t kfail[4] = { 0, 0, 0, 0 };
    const int ktry[4] = { 4, 8, 16, 32 };
    uint64_t *bucket_cyc[5];
    uint32_t bucket_n[5] = { 0, 0, 0, 0, 0 };
    dlmm_cache_t *cache;
    uint32_t n_cross;
    uint32_t b;

    recs = calloc(SYNTH_N, sizeof(*recs));
    crossed = calloc(SYNTH_N, sizeof(*crossed));
    bytes = calloc(SYNTH_N, sizeof(*bytes));
    cache = calloc(1, sizeof(*cache));
    for (b = 0; b < 5; b++) {
        bucket_cyc[b] = calloc(BENCH_ITERS, sizeof(uint64_t));
    }
    if (recs == NULL || crossed == NULL || bytes == NULL || cache == NULL
        || bucket_cyc[0] == NULL || bucket_cyc[1] == NULL || bucket_cyc[2] == NULL
        || bucket_cyc[3] == NULL || bucket_cyc[4] == NULL) {
        return -1;
    }
    for (i = 0; i < SYNTH_N; i++) {
        make_synth_rec(&recs[i], i % 40u, pick_walk(i), (uint8_t)(i & 1u));
    }
    if (dlmm_cap_save("/tmp/core004.synth.cap", recs, SYNTH_N) != 0) {
        fprintf(stderr, "cap save failed\n");
        return -1;
    }
    {
        dlmm_cap_rec_t *loaded = NULL;
        uint32_t nload = 0;
        if (dlmm_cap_load("/tmp/core004.synth.cap", &loaded, &nload) != 0
            || nload != SYNTH_N) {
            fprintf(stderr, "cap reload failed\n");
            return -1;
        }
        memcpy(recs, loaded, (size_t)nload * sizeof(*recs));
        dlmm_cap_free(loaded);
    }

    n_cross = 0;
    for (i = 0; i < SYNTH_N; i++) {
        dlmm_cap_verdict_t v;
        int k;

        if (dlmm_cap_check(&recs[i], &v) != 0) {
            fprintf(stderr, "cap_check rec %u\n", i);
            return -1;
        }
        if (v.insufficient) {
            insuff++;
        } else if (v.match) {
            match++;
        } else {
            mismatch++;
        }
        crossed[n_cross] = v.crossed;
        bytes[n_cross] = v.hot_bytes;
        n_cross++;
        for (k = 0; k < 4; k++) {
            if ((int)v.crossed > ktry[k]) {
                kfail[k]++;
            }
        }
    }

    qsort(crossed, n_cross, sizeof(*crossed), cmp_u32);
    qsort(bytes, n_cross, sizeof(*bytes), cmp_u32);

    printf("prediction\n");
    printf("  records          %u\n", SYNTH_N);
    printf("  predict(S,N)=S'  %u\n", match);
    printf("  insufficient     %u\n", insuff);
    printf("  mismatch         %u\n", mismatch);
    printf("  source           synthetic (protocol apply as after)\n\n");

    printf("bins crossed\n");
    printf("  p50    %u\n", pct_u32(crossed, n_cross, 0.50));
    printf("  p90    %u\n", pct_u32(crossed, n_cross, 0.90));
    printf("  p99    %u\n", pct_u32(crossed, n_cross, 0.99));
    printf("  p99.9  %u\n", pct_u32(crossed, n_cross, 0.999));
    printf("  max    %u\n\n", crossed[n_cross - 1]);

    printf("required cached bytes (scalars + live bins)\n");
    printf("  p50    %u\n", pct_u32(bytes, n_cross, 0.50));
    printf("  p99    %u\n", pct_u32(bytes, n_cross, 0.99));
    printf("  max    %u\n", bytes[n_cross - 1]);
    printf("  sizeof(hot) %zu   window ±%d = %d bins\n\n",
           sizeof(dlmm_pool_hot_t), DLMM_CACHE_K, DLMM_CACHE_WINDOW);

    printf("K sufficiency (%% of these swaps that walk past ±K)\n");
    for (i = 0; i < 4; i++) {
        printf("  K=%-2d   fail %u / %u\n", ktry[i], kfail[i], SYNTH_N);
    }
    printf("\n");

    dlmm_cache_init(cache);
    for (i = 0; i < 5; i++) {
        dlmm_state_t st;
        dlmm_swap_ix_t ix;
        dlmm_pool_hot_t spec;
        dlmm_apply_result_t res;
        uint16_t target = (i == 0) ? 0 : (i == 1) ? 1 : (i == 2) ? 3 : (i == 3) ? 6 : 12;
        uint64_t it;
        uint64_t loop;

        fill_walk_state(&st, target, 1);
        ix.min_amount_out = 0;
        ix.swap_for_y = 1;
        ix.amount_in = (target == 0) ? 80ull : ((uint64_t)target * 3000ull + 500ull);
        if (dlmm_cache_put(cache, i, &st) != 0) {
            return -1;
        }
        if (dlmm_cache_predict(cache, i, &ix, &spec, &res) != 0) {
            fprintf(stderr, "bench setup predict fail bucket %u\n", i);
            return -1;
        }
        for (it = 0; it < BENCH_WARMUP; it++) {
            (void)dlmm_cache_predict(cache, i, &ix, &spec, &res);
        }
        for (loop = 0; loop < loops; loop++) {
            for (it = 0; it < BENCH_ITERS && bucket_n[i] < BENCH_ITERS; it++) {
                uint64_t t0, t1;
                t0 = rdtscp();
                (void)dlmm_cache_predict(cache, i, &ix, &spec, &res);
                t1 = rdtscp();
                if (t1 >= t0) {
                    bucket_cyc[i][bucket_n[i]++] = t1 - t0;
                }
            }
        }
        (void)target;
    }

    printf("predict cycles by walk\n");
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

    free(recs);
    free(crossed);
    free(bytes);
    free(cache);
    for (b = 0; b < 5; b++) {
        free(bucket_cyc[b]);
    }
    return (mismatch == 0 && insuff == 0) ? 0 : -1;
}

static int
run_file(const char *path)
{
    dlmm_cap_rec_t *recs = NULL;
    uint32_t n = 0;
    uint32_t i;
    uint32_t match = 0;
    uint32_t insuff = 0;
    uint32_t mismatch = 0;
    uint32_t *crossed;
    uint32_t *bytes;

    if (dlmm_cap_load(path, &recs, &n) != 0) {
        fprintf(stderr, "failed to load %s\n", path);
        return -1;
    }
    if (n == 0) {
        printf("live cap %s: 0 records\n", path);
        dlmm_cap_free(recs);
        return 0;
    }
    crossed = calloc(n, sizeof(*crossed));
    bytes = calloc(n, sizeof(*bytes));
    if (crossed == NULL || bytes == NULL) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        dlmm_cap_verdict_t v;
        if (dlmm_cap_check(&recs[i], &v) != 0) {
            fprintf(stderr, "live rec %u check failed\n", i);
            return -1;
        }
        if (v.insufficient) {
            insuff++;
        } else if (v.match) {
            match++;
        } else {
            mismatch++;
        }
        crossed[i] = v.crossed;
        bytes[i] = v.hot_bytes;
    }
    qsort(crossed, n, sizeof(*crossed), cmp_u32);
    qsort(bytes, n, sizeof(*bytes), cmp_u32);
    printf("\nlive capture %s\n", path);
    printf("  records          %u\n", n);
    printf("  predict(S,N)=S'  %u\n", match);
    printf("  insufficient     %u\n", insuff);
    printf("  mismatch         %u\n", mismatch);
    printf("  crossed p50/p99/max  %u / %u / %u\n",
           pct_u32(crossed, n, 0.50), pct_u32(crossed, n, 0.99), crossed[n - 1]);
    printf("  bytes   p50/p99/max  %u / %u / %u\n",
           pct_u32(bytes, n, 0.50), pct_u32(bytes, n, 0.99), bytes[n - 1]);
    free(crossed);
    free(bytes);
    dlmm_cap_free(recs);
    return mismatch == 0 ? 0 : -1;
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
    fails += check_missing_bin() != 0;
    fails += check_window_edge() != 0;
    fails += check_direct_index() != 0;
    if (fails) {
        printf("CORE-004  FAIL  contracts %d\n", fails);
        return 1;
    }

    printf("CORE-004\n\n");
    printf("  speculative isolation    ok  canonical S untouched\n");
    printf("  missing bin              ok  FAIL closed\n");
    printf("  window ±%d               ok  walk 16 fit / 17 miss\n", DLMM_CACHE_K);
    printf("  pools[pool_idx]          ok  direct, no hash\n\n");

    if (run_synth(&tsc, o.loops) != 0) {
        printf("\nCORE-004  FAIL  synthetic replay\n");
        return 1;
    }
    if (o.cap != NULL && run_file(o.cap) != 0) {
        return 1;
    } else if (o.cap == NULL) {
        printf("\nlive before/after triples: none  (recorder writes .cap, replay with --cap)\n");
    }
    return 0;
}
