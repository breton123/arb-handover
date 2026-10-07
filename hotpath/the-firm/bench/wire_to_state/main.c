#include "deps/class.h"
#include "net/time.h"
#include "state/compact.h"
#include "wire/mkpkt.h"
#include "wire/pipeline.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARM  400u
#define ITERS 4000u

static const char *const STN[WIRE_ST_N] = {
    "packet_rx",
    "tx_complete",
    "OrderedTx",
    "resolve",
    "deps_classify",
    "prestate_ready",
    "overlay/apply",
    "exactness_cert",
    "atomic_publish",
    "e2e",
};

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void
report(const char *name, uint64_t *s, uint32_t n)
{
    uint32_t i;
    uint64_t sum = 0;

    if (n == 0) {
        printf("  %-18s  (no samples)\n", name);
        return;
    }
    qsort(s, n, sizeof(*s), cmp_u64);
    for (i = 0; i < n; i++) {
        sum += s[i];
    }
    printf("  %-18s  min %" PRIu64 "  p50 %" PRIu64 "  p90 %" PRIu64
           "  p99 %" PRIu64 "  max %" PRIu64 "  mean %.1f ns\n",
           name, s[0], s[n / 2], s[(n * 90u) / 100u], s[(n * 99u) / 100u],
           s[n - 1], (double)sum / (double)n);
}

static void
seed(compact_state_t *st, const uint8_t pool[32], uint8_t auth)
{
    pump_state_t p;
    uint32_t id = 0;

    compact_state_clear(st);
    memset(&p, 0, sizeof(p));
    p.reserve_base = 10000000;
    p.reserve_quote = 10000000;
    p.lp_fee_bps = 20;
    (void)compact_pool_add_pump(st, pool, &p, &id);
    st->pool[id].auth_bits = auth;
}

int
main(void)
{
    compact_state_t *st;
    net_tsc_clock_t tsc;
    uint8_t pool[32], raw[256], pkt[400];
    uint32_t txl, i, n = 0, k;
    uint16_t plen;
    uint64_t *acc[WIRE_ST_N];
    uint64_t *exact_e2e;
    uint32_t n_exact_lat = 0;
    uint32_t n_fast = 0, n_fb = 0, n_unk = 0;
    uint32_t n_tx_first = 0, failc[16];
    uint64_t recon = 0, certn = 0, appn = 0;

    memset(failc, 0, sizeof(failc));
    if (net_tsc_calibrate(&tsc) != 0) {
        return 1;
    }
    st = calloc(1, sizeof(*st));
    exact_e2e = calloc(ITERS, sizeof(uint64_t));
    if (st == NULL || exact_e2e == NULL) {
        return 1;
    }
    for (k = 0; k < WIRE_ST_N; k++) {
        acc[k] = calloc(ITERS, sizeof(uint64_t));
        if (acc[k] == NULL) {
            return 1;
        }
    }
    memset(pool, 0x51, 32);
    txl = wire_mk_pump_sell(raw, sizeof(raw), pool, 25);
    plen = wire_wrap_shred(pkt, sizeof(pkt), 100, raw, txl);

    for (i = 0; i < WARM + ITERS; i++) {
        wire_in_t in;
        wire_out_t out;
        uint8_t auth = POOL_AUTH_READY;
        uint64_t t0;

        if ((i % 10u) == 0) {
            auth = (uint8_t)(POOL_AUTH_READY & ~POOL_AUTH_VQ);
        }
        seed(st, pool, auth);
        t0 = net_rdtscp();
        memset(&in, 0, sizeof(in));
        in.pkt = pkt;
        in.pkt_len = plen;
        in.tsc_rx = t0;
        in.st = st;
        if ((i % 5u) == 1) {
            /* AUTH arrives after tx bytes */
            in.tsc_pre_ready = t0 + 3000;
        }
        if (wire_to_state(&in, &out, tsc.hz) != 0) {
            continue;
        }
        if (i < WARM) {
            continue;
        }
        for (k = 0; k < WIRE_ST_N; k++) {
            acc[k][n] = out.ns[k];
        }
        if (out.admit == DEP_FAST_CUSTOM && out.fail == WIRE_FAIL_NONE) {
            n_fast++;
            exact_e2e[n_exact_lat++] = out.ns[WIRE_ST_E2E];
        } else if (out.admit == DEP_UNKNOWN) {
            n_unk++;
        } else {
            n_fb++;
        }
        if (out.fail < 16) {
            failc[out.fail]++;
        }
        if (out.tx_before_pre) {
            n_tx_first++;
        }
        recon += out.ns[WIRE_ST_TX_COMPLETE] + out.ns[WIRE_ST_ORDERED];
        certn += out.ns[WIRE_ST_RESOLVE] + out.ns[WIRE_ST_CLASSIFY]
            + out.ns[WIRE_ST_PRESTATE] + out.ns[WIRE_ST_CERT];
        appn += out.ns[WIRE_ST_APPLY] + out.ns[WIRE_ST_PUBLISH];
        n++;
    }

    printf("WIRE-TO-STATE-001  packet→tx→OrderedTx→resolve→deps→pre→apply→cert→publish\n");
    printf("samples=%u  tsc_hz=%.6e\n", n, tsc.hz);
    printf("FAST_CUSTOM=%u  FALLBACK=%u  UNKNOWN=%u\n", n_fast, n_fb, n_unk);
    printf("tx_bytes_before_prestate=%u (%.2f%%)\n", n_tx_first,
           n ? 100.0 * (double)n_tx_first / (double)n : 0.0);
    printf("fail reasons:\n");
    {
        uint32_t f;
        for (f = 0; f < 16; f++) {
            if (failc[f]) {
                printf("  %-16s %u\n", wire_fail_name((uint8_t)f), failc[f]);
            }
        }
    }
    printf("budget (sum of stage ns over samples):\n");
    printf("  reconstruction     %" PRIu64 "  (tx_complete+OrderedTx)\n", recon);
    printf("  state-certification %" PRIu64 "  (resolve+classify+pre+cert)\n",
           certn);
    printf("  apply+publish      %" PRIu64 "\n", appn);
    printf("stages (all candidates):\n");
    for (k = 0; k < WIRE_ST_N; k++) {
        report(STN[k], acc[k], n);
    }
    printf("exact-state e2e (FAST_CUSTOM only):\n");
    report("exact_e2e", exact_e2e, n_exact_lat);
    printf("\nintra-process rdtscp. Synthetic DATA_COMPLETE shred, not NIC RTT.\n");
    printf("DLMM not exercised.\n");
    free(st);
    free(exact_e2e);
    for (k = 0; k < WIRE_ST_N; k++) {
        free(acc[k]);
    }
    return 0;
}
