#include "pump.h"
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
    const char *vectors;
};

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--cpu N] [--mlock] [--loops N] [--vectors FILE]\n",
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
        } else if (strcmp(argv[i], "--vectors") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return -1;
            }
            o->vectors = argv[++i];
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static pump_state_t
base_pool(void)
{
    pump_state_t s;

    memset(&s, 0, sizeof(s));
    s.reserve_base = 1000000;
    s.reserve_quote = 1000000;
    s.lp_fee_bps = 20;
    s.protocol_fee_bps = 5;
    s.creator_fee_bps = 0;
    return s;
}

static int
quote_agrees_apply(const pump_state_t *s, uint64_t ain, uint8_t dir)
{
    pump_state_t snap;
    pump_state_t after;
    pump_swap_ix_t ix;
    pump_swap_result_t res;
    pump_quote_t q;
    int arc;
    int qrc;

    snap = *s;
    ix.amount_in = ain;
    ix.min_amount_out = 0;
    ix.direction = dir;
    arc = pump_apply_swap(s, &ix, &after, &res);
    qrc = pump_quote_exact_in(s, ain, dir, &q);
    if (memcmp(s, &snap, sizeof(snap)) != 0) {
        fprintf(stderr, "quote mutated S\n");
        return -1;
    }
    if (arc != 0) {
        if (qrc == 0 || q.valid) {
            fprintf(stderr, "quote ok but apply failed\n");
            return -1;
        }
        return 0;
    }
    if (qrc != 0 || !q.valid) {
        fprintf(stderr, "quote failed but apply ok\n");
        return -1;
    }
    if (q.amount_out != res.amount_out || q.fee != res.fee) {
        fprintf(stderr, "quote/apply mismatch out=%" PRIu64 "/%" PRIu64
                        " fee=%" PRIu64 "/%" PRIu64 "\n",
                q.amount_out, res.amount_out, q.fee, res.fee);
        return -1;
    }
    return 0;
}

static int
check_isolation(void)
{
    pump_state_t s = base_pool();
    pump_state_t snap = s;
    pump_quote_t q;

    if (pump_quote_exact_in(&s, 1000, PUMP_DIR_BASE_TO_QUOTE, &q) != 0
        || !q.valid || q.amount_out == 0) {
        fprintf(stderr, "isolation quote failed\n");
        return -1;
    }
    if (memcmp(&s, &snap, sizeof(s)) != 0) {
        fprintf(stderr, "isolation mutated S\n");
        return -1;
    }
    return 0;
}

static int
check_no_fee_sell(void)
{
    pump_state_t s;
    pump_quote_t q;

    memset(&s, 0, sizeof(s));
    s.reserve_base = 1000;
    s.reserve_quote = 1000;
    if (pump_quote_exact_in(&s, 100, PUMP_DIR_BASE_TO_QUOTE, &q) != 0
        || q.amount_out != 90 || q.fee != 0) {
        fprintf(stderr, "no-fee sell %" PRIu64 " fee %" PRIu64 "\n",
                q.amount_out, q.fee);
        return -1;
    }
    return 0;
}

static int
check_fail_closed(void)
{
    pump_state_t s = base_pool();
    pump_state_t snap;
    pump_quote_t q;

    s.reserve_base = 0;
    snap = s;
    if (pump_quote_exact_in(&s, 100, PUMP_DIR_BASE_TO_QUOTE, &q) == 0
        || q.valid) {
        fprintf(stderr, "zero reserve should fail\n");
        return -1;
    }
    if (memcmp(&s, &snap, sizeof(s)) != 0) {
        fprintf(stderr, "fail path mutated S\n");
        return -1;
    }

    s = base_pool();
    s.status = 1;
    if (pump_quote_exact_in(&s, 100, PUMP_DIR_QUOTE_TO_BASE, &q) == 0) {
        fprintf(stderr, "mayhem should fail\n");
        return -1;
    }
    s = base_pool();
    s.disabled = PUMP_DISABLE_BUY;
    if (pump_quote_exact_in(&s, 100, PUMP_DIR_QUOTE_TO_BASE, &q) == 0) {
        fprintf(stderr, "disabled buy should fail\n");
        return -1;
    }
    s.disabled = PUMP_DISABLE_SELL;
    if (pump_quote_exact_in(&s, 100, PUMP_DIR_BASE_TO_QUOTE, &q) == 0) {
        fprintf(stderr, "disabled sell should fail\n");
        return -1;
    }
    s = base_pool();
    {
        pump_swap_ix_t ix = {
            .amount_in = 100, .min_amount_out = 1000000,
            .direction = PUMP_DIR_BASE_TO_QUOTE
        };
        pump_state_t after;
        pump_swap_result_t res;
        if (pump_apply_swap(&s, &ix, &after, &res) == 0) {
            fprintf(stderr, "min_out should fail\n");
            return -1;
        }
    }
    return 0;
}

static int
check_apply_updates(void)
{
    pump_state_t s = base_pool();
    pump_state_t after;
    pump_swap_ix_t ix = {
        .amount_in = 1000, .min_amount_out = 0,
        .direction = PUMP_DIR_BASE_TO_QUOTE
    };
    pump_swap_result_t res;

    if (pump_apply_swap(&s, &ix, &after, &res) != 0) {
        fprintf(stderr, "sell apply failed\n");
        return -1;
    }
    if (after.reserve_base != s.reserve_base + 1000) {
        fprintf(stderr, "sell base reserve\n");
        return -1;
    }
    if (after.reserve_quote >= s.reserve_quote) {
        fprintf(stderr, "sell quote should fall\n");
        return -1;
    }
    if (after.virtual_quote != s.virtual_quote) {
        fprintf(stderr, "virtual mutated\n");
        return -1;
    }

    s = base_pool();
    ix.direction = PUMP_DIR_QUOTE_TO_BASE;
    if (pump_apply_swap(&s, &ix, &after, &res) != 0) {
        fprintf(stderr, "buy apply failed\n");
        return -1;
    }
    if (after.reserve_base >= s.reserve_base) {
        fprintf(stderr, "buy base should fall\n");
        return -1;
    }
    if (after.reserve_quote <= s.reserve_quote) {
        fprintf(stderr, "buy quote should rise\n");
        return -1;
    }
    return 0;
}

static int
run_vectors(const char *path)
{
    static const char *const fallback[] = {
        "tests/pump-vectors.txt",
        "../tests/pump-vectors.txt",
        "pump-vectors.txt",
    };
    FILE *f = NULL;
    char line[256];
    uint32_t n = 0;
    uint32_t i;

    if (path != NULL) {
        f = fopen(path, "r");
    }
    for (i = 0; f == NULL && i < 3; i++) {
        f = fopen(fallback[i], "r");
    }
    if (f == NULL) {
        fprintf(stderr, "pump vectors not found\n");
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long ain, base, quote, virt, lp, proto, cr, sell, expect;
        pump_state_t s;
        pump_quote_t q;
        uint8_t dir;

        if (line[0] < '0' || line[0] > '9') {
            continue;
        }
        if (sscanf(line,
                   "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &ain, &base, &quote, &virt, &lp, &proto, &cr, &sell,
                   &expect) != 9) {
            fprintf(stderr, "bad vector line %u\n", n);
            fclose(f);
            return -1;
        }
        memset(&s, 0, sizeof(s));
        s.reserve_base = (uint64_t)base;
        s.reserve_quote = (uint64_t)quote;
        s.virtual_quote = (int64_t)virt;
        s.lp_fee_bps = (uint64_t)lp;
        s.protocol_fee_bps = (uint64_t)proto;
        s.creator_fee_bps = (uint64_t)cr;
        dir = sell ? PUMP_DIR_BASE_TO_QUOTE : PUMP_DIR_QUOTE_TO_BASE;
        if (pump_quote_exact_in(&s, (uint64_t)ain, dir, &q) != 0 || !q.valid) {
            fprintf(stderr, "vector %u quote fail\n", n);
            fclose(f);
            return -1;
        }
        if (q.amount_out != (uint64_t)expect) {
            fprintf(stderr, "vector %u out %" PRIu64 " != %llu\n",
                    n, q.amount_out, expect);
            fclose(f);
            return -1;
        }
        if (quote_agrees_apply(&s, (uint64_t)ain, dir) != 0) {
            fprintf(stderr, "vector %u quote!=apply\n", n);
            fclose(f);
            return -1;
        }
        n++;
    }
    fclose(f);
    if (n != 10000) {
        fprintf(stderr, "expected 10000 vectors, got %u\n", n);
        return -1;
    }
    printf("official SDK vectors   %u / %u  exact amount_out\n", n, n);
    return 0;
}

static int
run_combos(void)
{
    uint32_t i;
    uint32_t agree = 0;
    uint32_t both_fail = 0;

    for (i = 0; i < COMBO_N; i++) {
        pump_state_t s;
        uint64_t ain;
        uint8_t dir = (uint8_t)(i & 1u);

        memset(&s, 0, sizeof(s));
        s.reserve_base = 1000000ull + (uint64_t)(i % 97) * 100000ull;
        s.reserve_quote = 2000000ull + (uint64_t)(i % 53) * 50000ull;
        s.virtual_quote = (i % 11 == 0) ? (int64_t)(i % 1000) : 0;
        s.lp_fee_bps = (i % 5 == 0) ? 0 : 20;
        s.protocol_fee_bps = (i % 7 == 0) ? 0 : 5;
        s.creator_fee_bps = (i % 3 == 0) ? 0 : 30;
        ain = 100ull + (uint64_t)(i % 1000) * 50ull;
        if ((i % 19u) == 0) {
            ain *= 100ull;
        }
        if (quote_agrees_apply(&s, ain, dir) != 0) {
            fprintf(stderr, "combo %u\n", i);
            return -1;
        }
        {
            pump_quote_t q;
            if (pump_quote_exact_in(&s, ain, dir, &q) == 0 && q.valid) {
                agree++;
            } else {
                both_fail++;
            }
        }
    }
    printf("quote == apply         %u agree / %u fail-closed / %u S unchanged\n",
           agree, both_fail, COMBO_N);
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
    static const char *const names[] = { "sell", "buy" };
    static const uint8_t dirs[] = {
        PUMP_DIR_BASE_TO_QUOTE, PUMP_DIR_QUOTE_TO_BASE
    };
    uint32_t d;

    printf("quote cycles\n");
    printf("  %-6s  %8s %8s %8s\n", "dir", "p50", "p99", "p999");
    for (d = 0; d < 2; d++) {
        pump_state_t s = base_pool();
        pump_quote_t q;
        uint64_t *cyc;
        uint32_t n = 0;
        uint64_t it;
        uint64_t loop;
        uint64_t p50, p99, p999;

        cyc = calloc(BENCH_ITERS, sizeof(uint64_t));
        if (cyc == NULL) {
            return -1;
        }
        if (pump_quote_exact_in(&s, 10000, dirs[d], &q) != 0 || !q.valid) {
            fprintf(stderr, "bench setup fail %s\n", names[d]);
            free(cyc);
            return -1;
        }
        for (it = 0; it < BENCH_WARMUP; it++) {
            (void)pump_quote_exact_in(&s, 10000, dirs[d], &q);
        }
        for (loop = 0; loop < loops; loop++) {
            for (it = 0; it < BENCH_ITERS && n < BENCH_ITERS; it++) {
                uint64_t t0, t1;
                t0 = rdtscp();
                (void)pump_quote_exact_in(&s, 10000, dirs[d], &q);
                t1 = rdtscp();
                if (t1 >= t0) {
                    cyc[n++] = t1 - t0;
                }
            }
        }
        qsort(cyc, n, sizeof(uint64_t), cmp_u64);
        p50 = pct_u64(cyc, n, 0.50);
        p99 = pct_u64(cyc, n, 0.99);
        p999 = pct_u64(cyc, n, 0.999);
        printf("  %-6s  %8" PRIu64 " %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " / %" PRIu64 " ns)\n",
               names[d], p50, p99, p999,
               tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99), tsc_to_ns(tsc, p999));
        free(cyc);
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
    fails += check_no_fee_sell() != 0;
    fails += check_fail_closed() != 0;
    fails += check_apply_updates() != 0;
    if (fails) {
        printf("CORE-006  FAIL  contracts %d\n", fails);
        return 1;
    }

    printf("CORE-006\n\n");
    printf("  quote isolation          ok  S bit-identical\n");
    printf("  no-fee sell              ok  1000/1000 + 100 → 90\n");
    printf("  fail closed              ok  zero / disabled / min_out / status\n");
    printf("  apply updates S'         ok  reserves move, virtual stays\n\n");

    if (run_vectors(o.vectors) != 0) {
        printf("CORE-006  FAIL  SDK vectors\n");
        return 1;
    }
    if (run_combos() != 0) {
        printf("CORE-006  FAIL  combinations\n");
        return 1;
    }
    if (run_bench(&tsc, o.loops) != 0) {
        printf("CORE-006  FAIL  bench\n");
        return 1;
    }
    return 0;
}
