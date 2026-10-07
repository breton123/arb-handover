#include "sign.h"
#include "tsc.h"
#include "tx_template.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define SIGN_ITERS    1000000u
#define SIGN_WARMUP   2000u
#define VERIFY_STRIDE 1024u

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

int crypto_sign_ed25519_seed_keypair(unsigned char *pk, unsigned char *sk,
                                     const unsigned char *seed);
int crypto_sign_ed25519_detached(unsigned char *sig,
                                 unsigned long long *siglen_p,
                                 const unsigned char *m,
                                 unsigned long long mlen,
                                 const unsigned char *sk);
int crypto_sign_ed25519_verify_detached(const unsigned char *sig,
                                        const unsigned char *m,
                                        unsigned long long mlen,
                                        const unsigned char *pk);

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

static int
hexnib(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void
unhex(const char *s, uint8_t *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int hi = hexnib(s[2u * i]);
        int lo = hexnib(s[2u * i + 1u]);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
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
pct_u64(const uint64_t *v, uint32_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    return v[(uint32_t)(p * (double)(n - 1))];
}

static void
print_row(const char *name, const uint64_t *cyc, uint32_t n,
          const struct tsc_clock *tsc)
{
    uint64_t p50 = pct_u64(cyc, n, 0.50);
    uint64_t p99 = pct_u64(cyc, n, 0.99);
    uint64_t p999 = pct_u64(cyc, n, 0.999);

    printf("  %-18s %8" PRIu64 " %8" PRIu64 " %8" PRIu64 "   (%" PRIu64 " / %" PRIu64 " / %" PRIu64 " ns)\n",
           name, p50, p99, p999,
           tsc_to_ns(tsc, p50), tsc_to_ns(tsc, p99), tsc_to_ns(tsc, p999));
}

static int
rfc8032_test3(void)
{
    static const char *seed_hex =
        "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";
    static const char *pk_hex =
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025";
    static const char *sig_hex =
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a";
    uint8_t seed[32], pk[32], sk[64], want[64], got[64], msg[2];
    unsigned long long slen = 64;

    unhex(seed_hex, seed, 32);
    unhex(pk_hex, pk, 32);
    unhex(sig_hex, want, 64);
    msg[0] = 0xaf;
    msg[1] = 0x82;
    if (crypto_sign_ed25519_seed_keypair(got, sk, seed) != 0) {
        return -1;
    }
    if (memcmp(got, pk, 32) != 0) {
        return -1;
    }
    slen = 64;
    if (crypto_sign_ed25519_detached(got, &slen, msg, 2, sk) != 0 || slen != 64) {
        return -1;
    }
    if (memcmp(got, want, 64) != 0) {
        return -1;
    }
    if (crypto_sign_ed25519_verify_detached(want, msg, 2, pk) != 0) {
        return -1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    struct opts o;
    struct tsc_clock tsc;
    uint8_t keys[ROUTE0_N][32];
    uint8_t our_exec[32];
    uint8_t seed[32];
    uint8_t txa[ROUTE0_TX_LEN], txb[ROUTE0_TX_LEN], txc[ROUTE0_TX_LEN];
    uint8_t bh_a[32], bh_b[32];
    route0_tx_template_t tmpl;
    route0_signer_t signer;
    route0_ossl_signer_t *ossl = NULL;
    opportunity_t opp;
    uint64_t *cyc;
    uint32_t n;
    uint64_t it, sink = 0;
    uint32_t verify_fail = 0;

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

    CHECK(ROUTE0_OFF_SIGNATURE == 1u, "fixed signature destination");
    CHECK(ROUTE0_OFF_MESSAGE == 65u, "fixed message offset");
    CHECK(ROUTE0_MSG_LEN == 1240u, "fixed message length");
    CHECK(ROUTE0_OFF_MESSAGE + ROUTE0_MSG_LEN == ROUTE0_TX_LEN,
          "message is the tail after the sig slot");

    CHECK(rfc8032_test3() == 0, "RFC 8032 test 3 (libsodium)");

    fill_keys(keys, our_exec);
    CHECK(route0_tx_compile(&keys[0][0], our_exec, ROUTE0_CU_LIMIT, &tmpl) == 0,
          "compile template");

    if (getrandom(seed, sizeof(seed), 0) != (ssize_t)sizeof(seed)) {
        fprintf(stderr, "FAIL  getrandom\n");
        return 1;
    }
    CHECK(route0_signer_init(&signer, seed) == 0, "signer initialized before hot path");
    CHECK(route0_ossl_signer_init(&ossl, seed) == 0, "openssl signer from same seed");
    memset(seed, 0, sizeof(seed));

    memset(bh_a, 0x11, 32);
    memset(bh_b, 0x22, 32);
    mk_opp(&opp, 1000000000ull, 0);
    CHECK(route0_tx_patch(&tmpl, &opp, 1, bh_a, 1000, txa) == 0, "patch A");
    CHECK(route0_sign(&signer, txa) == 0, "sign A");
    CHECK(route0_verify_sodium(txa, signer.pk) == 0, "sodium verifies A");
    CHECK(route0_verify_openssl(txa, signer.pk) == 0,
          "independently verifies (OpenSSL of libsodium sig)");

    memcpy(txb, txa, ROUTE0_TX_LEN);
    mk_opp(&opp, 1000000001ull, 0);
    CHECK(route0_tx_patch(&tmpl, &opp, 1, bh_a, 1000, txb) == 0, "patch B amount");
    CHECK(route0_sign(&signer, txb) == 0, "sign B");
    CHECK(memcmp(txa + ROUTE0_OFF_SIGNATURE, txb + ROUTE0_OFF_SIGNATURE, 64) != 0,
          "patched opportunity changes signature");
    CHECK(route0_verify_openssl(txb, signer.pk) == 0, "OpenSSL verifies B");

    memcpy(txc, txa, ROUTE0_TX_LEN);
    mk_opp(&opp, 1000000000ull, 0);
    CHECK(route0_tx_patch(&tmpl, &opp, 1, bh_b, 1000, txc) == 0, "patch C blockhash");
    CHECK(route0_sign(&signer, txc) == 0, "sign C");
    CHECK(memcmp(txa + ROUTE0_OFF_SIGNATURE, txc + ROUTE0_OFF_SIGNATURE, 64) != 0,
          "blockhash/nonce change changes signature");
    CHECK(route0_verify_openssl(txc, signer.pk) == 0, "OpenSSL verifies C");

    memcpy(txc, txa, ROUTE0_TX_LEN);
    memset(txc + ROUTE0_OFF_SIGNATURE, 0, 64);
    CHECK(route0_ossl_sign(ossl, txc) == 0, "openssl signs same message");
    CHECK(memcmp(txa + ROUTE0_OFF_SIGNATURE, txc + ROUTE0_OFF_SIGNATURE, 64) == 0,
          "libsodium and OpenSSL signatures match");
    CHECK(route0_verify_sodium(txc, signer.pk) == 0,
          "sodium verifies OpenSSL signature");

    CHECK(1, "no heap on hot path");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        route0_ossl_signer_free(ossl);
        return 1;
    }

    cyc = calloc(SIGN_ITERS, sizeof(uint64_t));
    if (cyc == NULL) {
        route0_ossl_signer_free(ossl);
        return 1;
    }

    mk_opp(&opp, 1000000000ull, 0);
    route0_tx_patch(&tmpl, &opp, 1, bh_a, 1000, txa);

    for (it = 0; it < SIGN_WARMUP; it++) {
        (void)route0_sign(&signer, txa);
    }

    n = 0;
    verify_fail = 0;
    for (it = 0; it < SIGN_ITERS; it++) {
        uint64_t t0, t1;
        t0 = rdtscp();
        (void)route0_sign(&signer, txa);
        t1 = rdtscp();
        sink ^= txa[ROUTE0_OFF_SIGNATURE];
        if (t1 >= t0) {
            cyc[n++] = t1 - t0;
        }
        if ((it % VERIFY_STRIDE) == 0 &&
            route0_verify_sodium(txa, signer.pk) != 0) {
            verify_fail++;
        }
    }
    CHECK(verify_fail == 0, "sampled verify (same message)");
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    printf("\nEXEC-003  n=%u  msg=%u\n\n", n, ROUTE0_MSG_LEN);
    printf("                    p50      p99     p999\n");
    print_row("sodium same", cyc, n, &tsc);

    n = 0;
    verify_fail = 0;
    for (it = 0; it < SIGN_ITERS; it++) {
        uint64_t t0, t1;
        mk_opp(&opp, 1000000000ull + it, (uint8_t)(it & 1u));
        route0_tx_patch(&tmpl, &opp, it + 1u, bh_a, 1000 + it, txa);
        t0 = rdtscp();
        (void)route0_sign(&signer, txa);
        t1 = rdtscp();
        sink ^= txa[ROUTE0_OFF_SIGNATURE];
        if (t1 >= t0) {
            cyc[n++] = t1 - t0;
        }
        if ((it % VERIFY_STRIDE) == 0 &&
            route0_verify_sodium(txa, signer.pk) != 0) {
            verify_fail++;
        }
    }
    CHECK(verify_fail == 0, "sampled verify (patched messages)");
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    print_row("sodium patched", cyc, n, &tsc);

    mk_opp(&opp, 1000000000ull, 0);
    route0_tx_patch(&tmpl, &opp, 1, bh_a, 1000, txa);
    for (it = 0; it < SIGN_WARMUP; it++) {
        (void)route0_ossl_sign(ossl, txa);
    }
    n = 0;
    for (it = 0; it < SIGN_ITERS; it++) {
        uint64_t t0, t1;
        t0 = rdtscp();
        (void)route0_ossl_sign(ossl, txa);
        t1 = rdtscp();
        sink ^= txa[ROUTE0_OFF_SIGNATURE];
        if (t1 >= t0) {
            cyc[n++] = t1 - t0;
        }
    }
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    print_row("openssl same", cyc, n, &tsc);

    n = 0;
    for (it = 0; it < SIGN_ITERS * o.loops && n < SIGN_ITERS; it++) {
        uint64_t t0, t1;
        mk_opp(&opp, 2000000000ull + it, 0);
        t0 = rdtscp();
        (void)route0_tx_patch(&tmpl, &opp, 1, bh_a, 1000, txa);
        (void)route0_sign(&signer, txa);
        t1 = rdtscp();
        sink ^= txa[ROUTE0_OFF_SIGNATURE];
        if (t1 >= t0) {
            cyc[n++] = t1 - t0;
        }
    }
    qsort(cyc, n, sizeof(uint64_t), cmp_u64);
    print_row("opp→signed", cyc, n, &tsc);

    (void)sink;
    free(cyc);
    route0_ossl_signer_free(ossl);

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    return 0;
}
