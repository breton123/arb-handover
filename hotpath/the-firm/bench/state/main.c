#include "ingress/tx.h"
#include "net/time.h"
#include "state/compact.h"
#include "transitions/apply.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARM   2000u
#define ITERS  20000u

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

static FILE *
open_vectors(void)
{
    static const char *const paths[] = {
        "tests/fixtures/pump/pump-vectors.txt",
        "../tests/fixtures/pump/pump-vectors.txt",
    };
    FILE *f = NULL;
    size_t i;

    for (i = 0; i < 2 && f == NULL; i++) {
        f = fopen(paths[i], "r");
    }
    return f;
}

static int
load_line(FILE *f, pump_state_t *s, uint64_t *ain, uint8_t *dir)
{
    char line[256];
    unsigned long long a, base, quote, virt, lp, proto, cr, sell, expect;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] < '0' || line[0] > '9') {
            continue;
        }
        if (sscanf(line, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &a, &base, &quote, &virt, &lp, &proto, &cr, &sell,
                   &expect) != 9) {
            return -1;
        }
        memset(s, 0, sizeof(*s));
        s->reserve_base = (uint64_t)base;
        s->reserve_quote = (uint64_t)quote;
        s->virtual_quote = (int64_t)virt;
        s->lp_fee_bps = (uint64_t)lp;
        s->protocol_fee_bps = (uint64_t)proto;
        s->creator_fee_bps = (uint64_t)cr;
        *ain = (uint64_t)a;
        *dir = sell ? PUMP_DIR_BASE_TO_QUOTE : PUMP_DIR_QUOTE_TO_BASE;
        (void)expect;
        return 0;
    }
    return 1;
}

int
main(void)
{
    FILE *f;
    compact_state_t st;
    ordered_tx_t tx;
    apply_result_t ar;
    net_tsc_clock_t tsc;
    uint64_t *samples;
    uint32_t i, n = 0;
    uint64_t sum = 0;

    if (net_tsc_calibrate(&tsc) != 0) {
        return 1;
    }
    f = open_vectors();
    if (f == NULL) {
        fprintf(stderr, "vectors not found\n");
        return 1;
    }
    samples = calloc(ITERS, sizeof(*samples));
    if (samples == NULL) {
        fclose(f);
        return 1;
    }

    for (i = 0; i < WARM + ITERS; i++) {
        pump_state_t seed;
        uint64_t ain, t0, t1;
        uint8_t dir;
        uint32_t pid = 0;

        if (load_line(f, &seed, &ain, &dir) != 0) {
            rewind(f);
            if (load_line(f, &seed, &ain, &dir) != 0) {
                break;
            }
        }
        compact_state_clear(&st);
        if (compact_pool_add_pump(&st, NULL, &seed, &pid) != 0) {
            break;
        }
        ordered_tx_clear(&tx);
        tx.slot = 1;
        tx.tx_index = i;
        tx.n_ix = 1;
        tx.ix[0].proto = PROTO_PUMP;
        tx.ix[0].kind = (dir == PUMP_DIR_BASE_TO_QUOTE) ? IX_KIND_PUMP_SELL
                                                        : IX_KIND_PUMP_BUY_EQ;
        tx.ix[0].relevant = 1;
        tx.ix[0].direction = dir;
        tx.ix[0].pool_id = pid;
        tx.ix[0].amount_in = ain;

        t0 = net_rdtscp();
        (void)apply_tx(&st, &tx, &ar);
        t1 = net_rdtscp();
        if (i >= WARM && n < ITERS) {
            samples[n] = net_tsc_to_ns(&tsc, t1 - t0);
            sum += samples[n];
            n++;
        }
    }
    fclose(f);

    if (n == 0) {
        free(samples);
        return 1;
    }
    qsort(samples, n, sizeof(*samples), cmp_u64);
    printf("STATE BENCH  apply_tx (classify + Pump overlay + publish)\n");
    printf("samples=%u  tsc_hz=%.6e\n", n, tsc.hz);
    printf("  min   %" PRIu64 " ns\n", samples[0]);
    printf("  p50   %" PRIu64 " ns\n", samples[n / 2]);
    printf("  p95   %" PRIu64 " ns\n", samples[(n * 95u) / 100u]);
    printf("  p99   %" PRIu64 " ns\n", samples[(n * 99u) / 100u]);
    printf("  max   %" PRIu64 " ns\n", samples[n - 1]);
    printf("  mean  %.1f ns\n", (double)sum / (double)n);
    printf("\nintra-process rdtscp. Target: warm apply << 1 ms.\n");
    free(samples);
    return 0;
}
