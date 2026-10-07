#include "deps/class.h"
#include "ingress/decode.h"
#include "net/capio.h"
#include "net/time.h"
#include "shred/assemble.h"
#include "state/compact.h"
#include "wire/pipeline.h"

#include "net/capture.h"
#include "wire/mkpkt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMP_MAX 200000u

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
        printf("  %-28s  (no samples)\n", name);
        return;
    }
    qsort(s, n, sizeof(*s), cmp_u64);
    for (i = 0; i < n; i++) {
        sum += s[i];
    }
    printf("  %-28s  n=%u  p50 %" PRIu64 "  p90 %" PRIu64 "  p99 %" PRIu64
           "  max %" PRIu64 "  mean %.1f ns\n",
           name, n, s[n / 2], s[(n * 90u) / 100u], s[(n * 99u) / 100u],
           s[n - 1], (double)sum / (double)n);
}

int
main(int argc, char **argv)
{
    capio_t cap;
    shred_assem_t *as = NULL;
    compact_state_t *st;
    const char *path = NULL;
    int self = 0;
    uint32_t max_pkts = 0, i;
    uint64_t *rx_tx, *rx_pub, *tx_pub, *rx_ds;
    uint32_t n_rxtx = 0, n_rxpub = 0, n_txpub = 0, n_ds = 0;
    uint32_t n_one = 0, n_multi = 0, n_fec = 0;
    uint32_t n_pump = 0, n_alt = 0, n_pre = 0, n_fast = 0, n_bad = 0;
    uint32_t n_tx = 0, n_empty = 0;
    uint32_t n_tx_first = 0, pkts = 0;
    double hz;
    uint32_t n;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--cap") == 0 && i + 1 < (uint32_t)argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--self-test") == 0) {
            self = 1;
        } else if (strcmp(argv[i], "--max-pkts") == 0 && i + 1 < (uint32_t)argc) {
            max_pkts = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }
    if (path == NULL && !self) {
        fprintf(stderr,
                "usage: wire_to_state002 --cap FILE [--max-pkts N]\n"
                "       wire_to_state002 --self-test\n");
        return 2;
    }
    if (self) {
        FILE *tf;
        netcap_hdr_t h;
        net_slot_t sl;
        uint8_t tx[256], p0[400], p1[400], pool[32];
        uint32_t tn, mid;
        uint16_t l0, l1;

#if defined(_WIN32)
        path = "firm_w2s002_self.cap";
#else
        path = "/tmp/firm_w2s002_self.cap";
#endif
        memset(pool, 0x44, 32);
        tn = wire_mk_pump_sell(tx, sizeof(tx), pool, 77);
        mid = tn / 2u;
        memset(p0, 0, sizeof(p0));
        memset(p1, 0, sizeof(p1));
        p0[SHRED_OFF_VARIANT] = SHRED_TYPE_MERKLE_DATA;
        p0[65] = 9;
        p0[73] = 10;
        {
            uint16_t sz = (uint16_t)mid;
            memcpy(p0 + 86, &sz, 2);
        }
        memcpy(p0 + SHRED_DATA_HDR_SZ, tx, mid);
        l0 = (uint16_t)(SHRED_DATA_HDR_SZ + mid);
        p1[SHRED_OFF_VARIANT] = SHRED_TYPE_MERKLE_DATA;
        p1[65] = 9;
        p1[73] = 11;
        p1[85] = 0x80;
        {
            uint16_t sz = (uint16_t)(tn - mid);
            memcpy(p1 + 86, &sz, 2);
        }
        memcpy(p1 + SHRED_DATA_HDR_SZ, tx + mid, tn - mid);
        l1 = (uint16_t)(SHRED_DATA_HDR_SZ + (tn - mid));
        tf = fopen(path, "wb");
        if (tf == NULL) {
            return 1;
        }
        memset(&h, 0, sizeof(h));
        h.tsc_hz = 3500000000ull;
        if (netcap_write_header(tf, &h) != 0) {
            fclose(tf);
            return 1;
        }
        memset(&sl, 0, sizeof(sl));
        sl.source_id = NET_SOURCE_ORBITFLARE;
        sl.rx_tsc = 1000;
        sl.len = l0;
        memcpy(sl.data, p0, l0);
        netcap_write_slot(tf, &sl);
        sl.rx_tsc = 1400;
        sl.seq = 1;
        sl.len = l1;
        memcpy(sl.data, p1, l1);
        netcap_write_slot(tf, &sl);
        fclose(tf);
    }
    if (capio_open(&cap, path) != 0) {
        fprintf(stderr, "open cap failed: %s\n", path);
        return 1;
    }
    hz = (cap.tsc_hz != 0) ? (double)cap.tsc_hz : 3.5e9;
    st = calloc(1, sizeof(*st));
    rx_tx = calloc(SAMP_MAX, sizeof(uint64_t));
    rx_pub = calloc(SAMP_MAX, sizeof(uint64_t));
    tx_pub = calloc(SAMP_MAX, sizeof(uint64_t));
    rx_ds = calloc(SAMP_MAX, sizeof(uint64_t));
    if (st == NULL || rx_tx == NULL || rx_pub == NULL || tx_pub == NULL
        || rx_ds == NULL || shred_assem_init(&as) != 0) {
        return 1;
    }
    compact_state_clear(st);

    while (max_pkts == 0 || pkts < max_pkts) {
        net_slot_t slot;
        shred_batch_t batch;
        int pr, rc;

        rc = capio_read(&cap, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            n_bad++;
            continue;
        }
        pkts++;
        pr = shred_assem_push(as, slot.data, slot.len, slot.rx_tsc, &batch);
        if (pr < 0) {
            continue;
        }
        if (pr == 0) {
            continue;
        }
        if (batch.how == RECON_ONE) {
            n_one++;
        } else if (batch.how == RECON_MULTI) {
            n_multi++;
        } else         if (batch.how == RECON_FEC) {
            n_fec++;
        }
        if (n_ds < SAMP_MAX && batch.tsc_done >= batch.tsc_first) {
            rx_ds[n_ds++] = (uint64_t)((double)(batch.tsc_done - batch.tsc_first)
                * 1e9 / hz);
        }
        n_tx += batch.n_tx;
        if (batch.n_tx == 0) {
            n_empty++;
        }
        if (batch.n_tx != 0 && n_rxtx < SAMP_MAX
            && batch.tsc_done >= batch.tsc_first) {
            rx_tx[n_rxtx++] = (uint64_t)((double)(batch.tsc_done
                - batch.tsc_first) * 1e9 / hz);
        }
        for (n = 0; n < batch.n_tx; n++) {
            wire_in_t in;
            wire_out_t out;
            const uint8_t *tb = batch.buf + batch.tx_off[n];
            uint32_t tl = batch.tx_len[n];
            ordered_tx_t probe;

            if (ingress_decode_tx(tb, tl, batch.slot, &probe) == DEC_ALT) {
                n_alt++;
                continue;
            }
            if (probe.n_ix == 0 || probe.ix[0].proto != PROTO_PUMP
                || (probe.ix[0].kind != IX_KIND_PUMP_SELL
                    && probe.ix[0].kind != IX_KIND_PUMP_BUY_EQ
                    && probe.ix[0].kind != IX_KIND_PUMP_BUY)) {
                continue;
            }
            n_pump++;
            memset(&in, 0, sizeof(in));
            in.framed = tb;
            in.framed_len = tl;
            in.tsc_rx = batch.tsc_first;
            in.tsc_tx_ready = batch.tsc_done;
            in.tsc_pre_ready = UINT64_MAX;
            in.st = st;
            if (wire_to_state(&in, &out, hz) != 0) {
                continue;
            }
            if (out.fail == WIRE_FAIL_ALT) {
                n_alt++;
            } else if (out.fail == WIRE_FAIL_PRE_VQ || out.fail == WIRE_FAIL_PRE_FEE
                       || out.fail == WIRE_FAIL_PRE_VAULT
                       || out.fail == WIRE_FAIL_PRE_MINT
                       || out.fail == WIRE_FAIL_RESOLVE) {
                n_pre++;
            } else if (out.fail == WIRE_FAIL_NONE
                       && out.admit == DEP_FAST_CUSTOM) {
                n_fast++;
                if (n_rxpub < SAMP_MAX && out.tsc_publish >= in.tsc_rx) {
                    rx_pub[n_rxpub++] = (uint64_t)((double)(out.tsc_publish
                        - in.tsc_rx) * 1e9 / hz);
                }
                if (n_txpub < SAMP_MAX && out.tsc_publish >= out.tsc_tx_ready) {
                    tx_pub[n_txpub++] = (uint64_t)((double)(out.tsc_publish
                        - out.tsc_tx_ready) * 1e9 / hz);
                }
            }
            if (out.tx_before_pre) {
                n_tx_first++;
            }
        }
    }

    printf("WIRE-TO-STATE-002  real OrbitFlare reconstruction\n");
    printf("cap=%s  kind=%s  tsc_hz=%.6e  pkts=%u\n", path,
           cap.kind == CAPIO_FEED ? "FEEDCAP1" : "FIRMCAP1", hz, pkts);
    printf("batches: one-packet=%u  multi-shred=%u  fec-recovered=%u  "
           "fec-possible=%u  incomplete-shreds=%u\n",
           n_one, n_multi, n_fec, shred_assem_fec_possible(as),
           shred_assem_incomplete(as));
    printf("framed txs=%u  empty-complete=%u  pump=%u  ALT=%u  "
           "prestate fallback=%u  FAST_CUSTOM=%u  tx_before_pre=%u\n",
           n_tx, n_empty, n_pump, n_alt, n_pre, n_fast, n_tx_first);
    printf("cap read errors=%u\n", n_bad);
    printf("\nheadline (capture TSC, not replay CPU):\n");
    report("packet_rx -> data_complete", rx_ds, n_ds);
    report("packet_rx -> tx_complete (all framed)", rx_tx, n_rxtx);
    report("packet_rx -> exact publish", rx_pub, n_rxpub);
    report("tx_complete -> exact publish", tx_pub, n_txpub);
    printf("\nFEC-recovered is 0 unless Reed-Solomon is implemented.\n");
    printf("FAST_CUSTOM requires certified AUTH in CompactState; "
           "caps carry shreds only.\n");
    printf("DLMM blocked.\n");
    capio_close(&cap);
    shred_assem_free(as);
    free(st);
    free(rx_tx);
    free(rx_pub);
    free(tx_pub);
    free(rx_ds);
    return 0;
}
