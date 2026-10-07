#include "net/time.h"
#include "shred/rs.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 32u
#define K 32u
#define COL 987u
#define ROUNDS 400u
#define WARM 20u

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void
report(uint32_t miss, uint64_t *s, uint32_t n)
{
    uint32_t i;
    uint64_t sum = 0;

    qsort(s, n, sizeof(*s), cmp_u64);
    for (i = 0; i < n; i++) {
        sum += s[i];
    }
    printf("  missing=%-2u  n=%u  p50 %" PRIu64 " ns  p99 %" PRIu64
           " ns  max %" PRIu64 " ns  mean %.1f ns\n",
           miss, n, s[n / 2], s[(n * 99u) / 100u], s[n - 1],
           (double)sum / (double)n);
}

int
main(void)
{
    static uint8_t store[N + K][COL];
    static uint8_t gold[N][COL];
    uint8_t *data[N], *code[K], *shards[N + K];
    uint8_t have[N + K];
    uint32_t miss_n[] = { 1, 2, 4, 8, 16, 32 };
    uint32_t m, r, i, c;
    uint64_t samp[ROUNDS];

    shred_rs_init();
    for (i = 0; i < N; i++) {
        data[i] = store[i];
        for (c = 0; c < COL; c++) {
            store[i][c] = (uint8_t)(i * 31u + c * 17u + 3u);
        }
        memcpy(gold[i], store[i], COL);
    }
    for (i = 0; i < K; i++) {
        code[i] = store[N + i];
    }
    if (shred_rs_encode(data, code, N, K, COL) != 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    printf("RS-FAST-001  n=%u k=%u cols=%u  cache_miss=%" PRIu64 "\n", N, K,
           COL, shred_rs_cache_misses());

    for (m = 0; m < sizeof(miss_n) / sizeof(miss_n[0]); m++) {
        uint32_t miss = miss_n[m];

        for (r = 0; r < WARM + ROUNDS; r++) {
            uint64_t t0, t1;

            memset(have, 1, sizeof(have));
            for (i = 0; i < N + K; i++) {
                shards[i] = store[i];
            }
            for (i = 0; i < miss; i++) {
                have[i] = 0;
                memset(store[i], 0x5a, COL);
            }
            t0 = net_now_ns();
            if (shred_rs_recover(shards, have, N, K, COL) != 0) {
                fprintf(stderr, "recover missing=%u failed\n", miss);
                return 1;
            }
            t1 = net_now_ns();
            if (r >= WARM) {
                samp[r - WARM] = t1 - t0;
            }
            for (i = 0; i < miss; i++) {
                if (memcmp(store[i], gold[i], COL) != 0) {
                    fprintf(stderr, "mismatch missing=%u shard=%u\n", miss, i);
                    return 1;
                }
            }
        }
        report(miss, samp, ROUNDS);
    }
    printf("cache hits=%" PRIu64 " misses=%" PRIu64 "\n", shred_rs_cache_hits(),
           shred_rs_cache_misses());
    return 0;
}
