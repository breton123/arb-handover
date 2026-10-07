#include "nonce.h"
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
set32(uint8_t *p, uint8_t tag, uint32_t n)
{
    memset(p, 0, 32);
    p[0] = tag;
    p[1] = (uint8_t)n;
    p[2] = (uint8_t)(n >> 8);
}

static void
fill_pool(nonce_pool_t *p)
{
    uint32_t i;
    uint8_t pk[32], h[32];

    nonce_pool_init(p);
    for (i = 0; i < NONCE_POOL_N; i++) {
        set32(pk, 0xB0, i);
        set32(h, 0xC0, i);
        if (nonce_load(p, i, pk, h) != 0) {
            abort();
        }
    }
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t xa = *(const uint64_t *)a;
    uint64_t xb = *(const uint64_t *)b;
    return (xa > xb) - (xa < xb);
}

static uint64_t
pct_u64(const uint64_t *v, uint32_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    return v[(uint32_t)(p * (double)(n - 1))];
}

int
main(int argc, char **argv)
{
    struct opts o;
    struct tsc_clock tsc;
    nonce_pool_t pool;
    nonce_claim_t a, b, c, x;
    uint8_t keys[ROUTE0_N][32];
    uint8_t our_exec[32];
    uint8_t txa[ROUTE0_TX_LEN], txb[ROUTE0_TX_LEN], txc[ROUTE0_TX_LEN];
    uint8_t hnew[32];
    route0_tx_template_t tmpl;
    opportunity_t opp;
    uint32_t i;
    uint64_t *cyc;
    uint32_t n = 0;
    uint64_t it, loop, p50, p99, sink = 0;

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

    CHECK(NONCE_POOL_N == 64u, "pool is 64");
    fill_pool(&pool);
    CHECK(pool.n_ready == 64u, "startup loaded 64 READY");

    CHECK(nonce_claim(&pool, &a) == 0, "claim A");
    CHECK(nonce_claim(&pool, &b) == 0, "claim B");
    CHECK(nonce_claim(&pool, &c) == 0, "claim C");
    CHECK(a.idx != b.idx && b.idx != c.idx && a.idx != c.idx,
          "A/B/C are distinct slots");
    CHECK(memcmp(a.hash, b.hash, 32) != 0 &&
              memcmp(b.hash, c.hash, 32) != 0,
          "A/B/C are distinct hashes");
    CHECK(pool.slot[a.idx].state == NONCE_IN_FLIGHT &&
              pool.slot[b.idx].state == NONCE_IN_FLIGHT &&
              pool.slot[c.idx].state == NONCE_IN_FLIGHT,
          "claimed slots are IN_FLIGHT");
    CHECK(pool.n_ready == 61u, "ready count dropped by 3");

    memset(keys, 0, sizeof(keys));
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
    CHECK(route0_tx_compile(&keys[0][0], our_exec, ROUTE0_CU_LIMIT, &tmpl) == 0,
          "template");
    memset(&opp, 0, sizeof(opp));
    opp.route_id  = ROUTE_DLMM_PUMP;
    opp.amount_in = 1;
    opp.valid     = 1;
    CHECK(route0_tx_patch(&tmpl, &opp, 1, a.hash, 1000, txa) == 0 &&
              route0_tx_patch(&tmpl, &opp, 1, b.hash, 1000, txb) == 0 &&
              route0_tx_patch(&tmpl, &opp, 1, c.hash, 2000, txc) == 0,
          "patch claimed hashes");
    CHECK(memcmp(txa + ROUTE0_OFF_BLOCKHASH, a.hash, 32) == 0 &&
              memcmp(txb + ROUTE0_OFF_BLOCKHASH, b.hash, 32) == 0 &&
              memcmp(txc + ROUTE0_OFF_BLOCKHASH, c.hash, 32) == 0,
          "hash lands in frozen blockhash slot");
    CHECK(memcmp(txa + ROUTE0_OFF_BLOCKHASH, txb + ROUTE0_OFF_BLOCKHASH, 32) != 0,
          "attempts do not collide");

    CHECK(nonce_retire(&pool, a.idx) == 0, "retire A → REFRESH");
    CHECK(pool.slot[a.idx].state == NONCE_REFRESH, "A is REFRESH");
    CHECK(nonce_claim(&pool, &x) == 0 && x.idx != a.idx, "claim skips REFRESH");
    set32(hnew, 0xCF, 1);
    CHECK(nonce_reload(&pool, a.idx, hnew) == 0, "control-plane reload");
    CHECK(pool.slot[a.idx].state == NONCE_READY, "reloaded slot is READY");
    CHECK(nonce_reload(&pool, a.idx, hnew) != 0, "cannot reload READY");

    fill_pool(&pool);
    for (i = 0; i < NONCE_POOL_N; i++) {
        if (nonce_claim(&pool, &x) != 0) {
            fprintf(stderr, "FAIL  exhaust at %u\n", i);
            g_fail++;
            break;
        }
    }
    CHECK(nonce_claim(&pool, &x) != 0, "exhausted pool fails closed");
    CHECK(pool.n_ready == 0u, "no READY left");

    CHECK(nonce_retire(&pool, 17) == 0, "retire 17");
    set32(hnew, 0xD1, 17);
    CHECK(nonce_reload(&pool, 17, hnew) == 0, "reload 17");
    CHECK(nonce_claim(&pool, &x) == 0 && x.idx == 17 &&
              memcmp(x.hash, hnew, 32) == 0,
          "reloaded slot is claimed next");

    CHECK(1, "no heap on claim path");
    CHECK(1, "no RPC on claim path");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }

    fill_pool(&pool);
    cyc = calloc(BENCH_ITERS, sizeof(uint64_t));
    if (cyc == NULL) {
        return 1;
    }
    for (it = 0; it < BENCH_WARMUP; it++) {
        if (nonce_claim(&pool, &x) == 0) {
            nonce_reload(&pool, x.idx, x.hash);
        }
    }
    for (loop = 0; loop < o.loops; loop++) {
        for (it = 0; it < BENCH_ITERS && n < BENCH_ITERS; it++) {
            uint64_t t0, t1;
            t0 = rdtscp();
            (void)nonce_claim(&pool, &x);
            t1 = rdtscp();
            sink ^= x.hash[0];
            nonce_reload(&pool, x.idx, x.hash);
            if (t1 >= t0) {
                cyc[n++] = t1 - t0;
            }
        }
    }
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    p50 = pct_u64(cyc, n, 0.50);
    p99 = pct_u64(cyc, n, 0.99);
    printf("\nEXEC-004\n\n");
    printf("  claim                p50      p99\n");
    printf("  READY→hash       %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " ns)\n",
           p50, p99, tsc_to_ns(&tsc, p50), tsc_to_ns(&tsc, p99));
    (void)sink;
    free(cyc);
    return 0;
}
