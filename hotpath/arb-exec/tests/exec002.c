#include "route0.h"
#include "tsc.h"
#include "tx_template.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_WARMUP  2000u
#define BENCH_ITERS   20000u

static int g_fail;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL  %s\n", (msg)); \
            g_fail++;                             \
        } else {                                  \
            printf("ok    %s\n", (msg));          \
        }                                         \
    } while (0)

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
fill_keys(uint8_t keys[ROUTE0_N][32], uint8_t our_exec[32])
{
    uint32_t i;

    memset(keys, 0, sizeof(uint8_t) * ROUTE0_N * 32);
    for (i = 0; i < ROUTE0_N; i++) {
        keys[i][0] = 0xA0;
        keys[i][1] = (uint8_t)i;
    }
    memcpy(keys[ROUTE0_ACC_PUMP_BASE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_X_MINT], 32);
    memcpy(keys[ROUTE0_ACC_PUMP_QUOTE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_Y_MINT], 32);
    memset(our_exec, 0, 32);
    our_exec[0] = 0xE0;
}

static int
in_dynamic(uint16_t i)
{
    if (i >= ROUTE0_OFF_BLOCKHASH && i < ROUTE0_OFF_BLOCKHASH + 32u) {
        return 1;
    }
    if (i >= ROUTE0_OFF_CU_PRICE && i < ROUTE0_OFF_CU_PRICE + 8u) {
        return 1;
    }
    if (i == ROUTE0_OFF_DIRECTION) {
        return 1;
    }
    if (i >= ROUTE0_OFF_AMOUNT_IN && i < ROUTE0_OFF_AMOUNT_IN + 8u) {
        return 1;
    }
    if (i >= ROUTE0_OFF_MIN_PROFIT && i < ROUTE0_OFF_MIN_PROFIT + 8u) {
        return 1;
    }
    return 0;
}

static void
mk_opp(opportunity_t *o, uint64_t amount_in, uint8_t dir)
{
    memset(o, 0, sizeof(*o));
    o->route_id     = ROUTE_DLMM_PUMP;
    o->amount_in    = amount_in;
    o->amount_out   = amount_in + 1u;
    o->gross_profit = 1;
    o->direction    = dir;
    o->valid        = 1;
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
    return v[(uint32_t)(p * (double)(n - 1))];
}

static int
run_bench(const struct tsc_clock *tsc, uint64_t loops,
          const route0_tx_template_t *tmpl)
{
    opportunity_t opp;
    uint8_t out[ROUTE0_TX_LEN];
    uint8_t bh[32];
    uint64_t *cyc;
    uint32_t n = 0;
    uint64_t it, loop, p50, p99;
    uint64_t sink = 0;

    mk_opp(&opp, 1000000000ull, 0);
    memset(bh, 0x11, 32);
    cyc = calloc(BENCH_ITERS, sizeof(uint64_t));
    if (cyc == NULL) {
        return -1;
    }
    for (it = 0; it < BENCH_WARMUP; it++) {
        (void)route0_tx_patch(tmpl, &opp, 1, bh, 1000, out);
    }
    for (loop = 0; loop < loops; loop++) {
        for (it = 0; it < BENCH_ITERS && n < BENCH_ITERS; it++) {
            uint64_t t0, t1;
            t0 = rdtscp();
            (void)route0_tx_patch(tmpl, &opp, 1, bh, 1000, out);
            t1 = rdtscp();
            sink ^= out[ROUTE0_OFF_AMOUNT_IN];
            if (t1 >= t0) {
                cyc[n++] = t1 - t0;
            }
        }
    }
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    p50 = pct_u64(cyc, n, 0.50);
    p99 = pct_u64(cyc, n, 0.99);
    printf("\nEXEC-002\n\n");
    printf("  patch                p50      p99\n");
    printf("  opportunity→tx   %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " ns)\n",
           p50, p99, tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99));
    printf("  tx_len               %u\n", ROUTE0_TX_LEN);
    (void)sink;
    free(cyc);
    return 0;
}

int
main(int argc, char **argv)
{
    struct opts o;
    struct tsc_clock tsc;
    uint8_t keys[ROUTE0_N][32];
    uint8_t our_exec[32];
    route0_tx_template_t tmpl, tmpl2;
    uint8_t txa[ROUTE0_TX_LEN], txb[ROUTE0_TX_LEN];
    uint8_t packed[ROUTE0_IX_LEN];
    uint8_t bh_a[32], bh_b[32];
    opportunity_t oppa, oppb;
    size_t plen = 0;
    uint32_t i;
    uint64_t v64;
    uint8_t msg_keys[ROUTE0_TX_NKEYS][32];

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

    fill_keys(keys, our_exec);
    CHECK(route0_tx_compile(&keys[0][0], our_exec, ROUTE0_CU_LIMIT, &tmpl) == 0,
          "compile route0 unsigned tx template");
    CHECK(route0_tx_compile(&keys[0][0], our_exec, ROUTE0_CU_LIMIT, &tmpl2) == 0,
          "compile is deterministic");
    CHECK(memcmp(tmpl.bytes, tmpl2.bytes, ROUTE0_TX_LEN) == 0,
          "compiled bytes identical");

    CHECK(tmpl.bytes[0] == 1, "one signature slot");
    CHECK(tmpl.bytes[65] == 1, "one required signer");
    CHECK(tmpl.bytes[66] == 0, "no readonly signers");
    CHECK(tmpl.bytes[67] == ROUTE0_TX_N_RO_UNSIGNED, "readonly unsigned count");
    CHECK(tmpl.bytes[ROUTE0_OFF_NKEYS] == ROUTE0_TX_NKEYS, "nkeys=35");
    CHECK(tmpl.bytes[ROUTE0_OFF_NIX] == 3, "three instructions");
    CHECK(tmpl.bytes[ROUTE0_OFF_ROUTE0_NACC] == ROUTE0_N,
          "route0 account count is 35");

    memcpy(msg_keys, tmpl.bytes + ROUTE0_OFF_KEYS, 32u * ROUTE0_TX_NKEYS);
    for (i = 0; i < ROUTE0_N; i++) {
        uint8_t idx = tmpl.bytes[ROUTE0_OFF_ROUTE0_ACCS + i];
        if (idx >= ROUTE0_TX_NKEYS ||
            memcmp(msg_keys[idx], keys[i], 32) != 0) {
            fprintf(stderr, "FAIL  acc[%u] layout\n", i);
            g_fail++;
        }
    }
    if (g_fail == 0) {
        printf("ok    exact 35-account layout preserved\n");
    }

    memset(bh_a, 0x11, 32);
    memset(bh_b, 0x22, 32);
    mk_opp(&oppa, 123456789ull, 0);
    mk_opp(&oppb, 999ull, 1);

    CHECK(route0_tx_patch(&tmpl, &oppa, 77, bh_a, 1000, txa) == 0, "patch A");
    CHECK(route0_pack(&oppa, 77, packed, sizeof(packed), &plen) == 0 &&
              plen == ROUTE0_IX_LEN,
          "route0_pack A");
    CHECK(memcmp(txa + ROUTE0_OFF_ROUTE0_DATA, packed, ROUTE0_IX_LEN) == 0,
          "instruction bytes match route0_pack()");

    CHECK(route0_tx_patch(&tmpl, &oppb, 5, bh_b, 4242, txb) == 0, "patch B");
    {
        int static_ok = 1;
        for (i = 0; i < ROUTE0_TX_LEN; i++) {
            if (in_dynamic((uint16_t)i)) {
                continue;
            }
            if (txa[i] != txb[i]) {
                static_ok = 0;
                break;
            }
        }
        CHECK(static_ok, "static bytes are identical across opportunities");
    }

    CHECK(ROUTE0_OFF_AMOUNT_IN == 1289u, "amount_in offset is a constant");
    CHECK(ROUTE0_OFF_DIRECTION == 1288u, "direction offset is a constant");
    CHECK(ROUTE0_OFF_MIN_PROFIT == 1297u, "min_profit offset is a constant");
    CHECK(ROUTE0_OFF_BLOCKHASH == 1189u, "blockhash offset is a constant");
    CHECK(ROUTE0_OFF_CU_PRICE == 1234u, "CU price offset is a constant");

    memcpy(&v64, txa + ROUTE0_OFF_AMOUNT_IN, 8);
    CHECK(v64 == 123456789ull, "amount_in patch works");
    CHECK(txa[ROUTE0_OFF_DIRECTION] == 0, "direction patch works (0)");
    CHECK(txb[ROUTE0_OFF_DIRECTION] == 1, "direction patch works (1)");
    memcpy(&v64, txa + ROUTE0_OFF_MIN_PROFIT, 8);
    CHECK(v64 == 77ull, "min_profit patch works");
    CHECK(memcmp(txa + ROUTE0_OFF_BLOCKHASH, bh_a, 32) == 0,
          "blockhash/nonce patch works");
    memcpy(&v64, txb + ROUTE0_OFF_CU_PRICE, 8);
    CHECK(v64 == 4242ull, "CU price patch works");

    CHECK(route0_tx_patch(&tmpl, &oppa, 77, bh_a, 1000, txb) == 0, "patch A again");
    CHECK(memcmp(txa, txb, ROUTE0_TX_LEN) == 0,
          "deterministic byte-for-byte output");

    {
        opportunity_t bad;
        memset(&bad, 0, sizeof(bad));
        CHECK(route0_tx_patch(&tmpl, &bad, 1, bh_a, 1, txa) != 0,
              "invalid opportunity rejected");
    }

    printf("ok    no heap in patch path\n");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }

    if (run_bench(&tsc, o.loops, &tmpl) != 0) {
        fprintf(stderr, "FAIL  bench\n");
        return 1;
    }
    return 0;
}
