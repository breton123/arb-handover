#include "deps/class.h"
#include "ingress/decode.h"
#include "net/capio.h"
#include "net/capture.h"
#include "net/time.h"
#include "shred/assemble.h"
#include "state/compact.h"
#include "wire/classify.h"
#include "wire/mkpkt.h"
#include "wire/pipeline.h"

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
        printf("  %-36s  (no samples)\n", name);
        return;
    }
    qsort(s, n, sizeof(*s), cmp_u64);
    for (i = 0; i < n; i++) {
        sum += s[i];
    }
    printf("  %-36s  n=%u  p50 %" PRIu64 "  p90 %" PRIu64 "  p99 %" PRIu64
           "  max %" PRIu64 "  mean %.1f ns\n",
           name, n, s[n / 2], s[(n * 90u) / 100u], s[(n * 99u) / 100u],
           s[n - 1], (double)sum / (double)n);
}

static int
ok_tsc(uint64_t a, uint64_t b)
{
    return a != 0 && b >= a && (b - a) < (1ull << 40);
}

static void
hexsig(char *o, const uint8_t *s)
{
    static const char *h = "0123456789abcdef";
    uint32_t i;

    for (i = 0; i < 64u; i++) {
        o[2u * i] = h[s[i] >> 4];
        o[2u * i + 1u] = h[s[i] & 0x0fu];
    }
    o[128] = 0;
}

static void
hexn(char *o, const uint8_t *s, uint32_t n)
{
    static const char *h = "0123456789abcdef";
    uint32_t i;

    for (i = 0; i < n; i++) {
        o[2u * i] = h[s[i] >> 4];
        o[2u * i + 1u] = h[s[i] & 0x0fu];
    }
    o[2u * n] = 0;
}

static const char *
disc_name(const uint8_t *d)
{
    static const uint8_t sell[8] = {
        0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
    };
    static const uint8_t buyeq[8] = {
        0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70
    };
    static const uint8_t buyout[8] = {
        0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
    };

    if (memcmp(d, sell, 8) == 0) {
        return "amm_sell";
    }
    if (memcmp(d, buyeq, 8) == 0) {
        return "amm_buy_exact_quote";
    }
    if (memcmp(d, buyout, 8) == 0) {
        return "amm_buy_exact_out";
    }
    return "other";
}

int
main(int argc, char **argv)
{
    capio_t cap;
    shred_assem_t *as = NULL;
    compact_state_t *st;
    const shred_fec_stats_t *fs;
    const shred_stream_stats_t *ss;
    const char *path = NULL, *dump_path = NULL, *pump_path = NULL;
    FILE *dump = NULL, *pumpf = NULL;
    wire_class_stats_t cs;
    int self = 0;
    uint32_t max_pkts = 0, i;
    uint64_t *rx_tx, *rx_pub, *tx_pub, *rx_ds, *rx_en, *rx_co, *rx_ent;
    uint32_t n_rxtx = 0, n_rxpub = 0, n_txpub = 0, n_ds = 0, n_en = 0;
    uint32_t n_co = 0, n_entc = 0;
    uint32_t n_one = 0, n_multi = 0, n_fec = 0, n_sets = 0;
    uint32_t n_pump = 0, n_alt = 0, n_pre = 0, n_fast = 0, n_bad = 0;
    uint32_t n_tx = 0, n_empty = 0, n_tx_first = 0, pkts = 0;
    double hz;
    uint32_t n;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--cap") == 0 && i + 1 < (uint32_t)argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--self-test") == 0) {
            self = 1;
        } else if (strcmp(argv[i], "--max-pkts") == 0 && i + 1 < (uint32_t)argc) {
            max_pkts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--dump-txs") == 0
                   && i + 1 < (uint32_t)argc) {
            dump_path = argv[++i];
        } else if (strcmp(argv[i], "--dump-pump") == 0
                   && i + 1 < (uint32_t)argc) {
            pump_path = argv[++i];
        }
    }
    if (path == NULL && !self) {
        fprintf(stderr,
                "usage: wire_to_state003 --cap FILE [--max-pkts N] "
                "[--dump-txs FILE] [--dump-pump FILE]\n"
                "       wire_to_state003 --self-test\n");
        return 2;
    }
    if (self) {
        fprintf(stderr, "self-test: use --cap on a FEEDCAP1 file\n");
        return 0;
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
    rx_en = calloc(SAMP_MAX, sizeof(uint64_t));
    rx_co = calloc(SAMP_MAX, sizeof(uint64_t));
    rx_ent = calloc(SAMP_MAX, sizeof(uint64_t));
    if (st == NULL || rx_tx == NULL || rx_pub == NULL || tx_pub == NULL
        || rx_ds == NULL || rx_en == NULL || rx_co == NULL || rx_ent == NULL
        || shred_assem_init(&as) != 0) {
        return 1;
    }
    compact_state_clear(st);
    memset(&cs, 0, sizeof(cs));
    if (dump_path != NULL) {
        dump = fopen(dump_path, "w");
        if (dump == NULL) {
            fprintf(stderr, "dump open failed: %s\n", dump_path);
            return 1;
        }
    }
    if (pump_path != NULL) {
        pumpf = fopen(pump_path, "w");
        if (pumpf == NULL) {
            fprintf(stderr, "dump-pump open failed: %s\n", pump_path);
            return 1;
        }
    }

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
        while (pr == 1) {
            n_sets++;
            if (batch.how == RECON_ONE) {
                n_one++;
            } else if (batch.how == RECON_MULTI) {
                n_multi++;
            } else if (batch.how == RECON_FEC) {
                n_fec++;
            }
            if (n_ds < SAMP_MAX && ok_tsc(batch.tsc_first, batch.tsc_done)) {
                rx_ds[n_ds++] = (uint64_t)((double)(batch.tsc_done
                    - batch.tsc_first) * 1e9 / hz);
            }
            if (n_en < SAMP_MAX && ok_tsc(batch.tsc_first, batch.tsc_enough)) {
                rx_en[n_en++] = (uint64_t)((double)(batch.tsc_enough
                    - batch.tsc_first) * 1e9 / hz);
            }
            if (n_co < SAMP_MAX && ok_tsc(batch.tsc_first, batch.tsc_contig)) {
                rx_co[n_co++] = (uint64_t)((double)(batch.tsc_contig
                    - batch.tsc_first) * 1e9 / hz);
            }
            if (n_entc < SAMP_MAX && ok_tsc(batch.tsc_first, batch.tsc_entry)) {
                rx_ent[n_entc++] = (uint64_t)((double)(batch.tsc_entry
                    - batch.tsc_first) * 1e9 / hz);
            }
            n_tx += batch.n_tx;
            if (batch.n_tx == 0) {
                n_empty++;
            }
            if (batch.n_tx != 0 && n_rxtx < SAMP_MAX
                && ok_tsc(batch.tsc_first, batch.tsc_done)) {
                rx_tx[n_rxtx++] = (uint64_t)((double)(batch.tsc_done
                    - batch.tsc_first) * 1e9 / hz);
            }
            for (n = 0; n < batch.n_tx; n++) {
                wire_in_t in;
                wire_out_t out;
                const uint8_t *tb = batch.buf + batch.tx_off[n];
                uint32_t tl = batch.tx_len[n];
                ordered_tx_t probe;
                wire_class_t cl;
                char sighex[129];

                if (wire_classify(tb, tl, NULL, &cl) != 0) {
                    continue;
                }
                cs.verified++;
                if (cl.klass == WIRE_CL_DIRECT_PUMP) {
                    cs.direct_pump++;
                } else if (cl.klass == WIRE_CL_DIRECT_DLMM) {
                    cs.direct_dlmm++;
                } else if (cl.klass == WIRE_CL_KNOWN_ROUTER) {
                    cs.known_router++;
                } else if (cl.klass == WIRE_CL_WATCHED) {
                    cs.watched++;
                } else if (cl.klass == WIRE_CL_UNRESOLVED_ALT) {
                    cs.unresolved_alt++;
                } else {
                    cs.other++;
                }
                if (dump != NULL) {
                    hexsig(sighex, cl.sig);
                    fprintf(dump,
                            "{\"slot\":%" PRIu64 ",\"entry\":%u,\"tx_i\":%u,"
                            "\"sig_hex\":\"%s\",\"version\":%u,\"class\":%u}\n",
                            batch.slot, batch.tx_entry[n], batch.tx_ord[n],
                            sighex, cl.encoding, cl.klass);
                }
                if (pumpf != NULL && cl.klass == WIRE_CL_DIRECT_PUMP) {
                    wire_ix_view_t ixs[WIRE_IX_VIEW_MAX];
                    uint32_t nix = 0, k;
                    char phex[65], dhex[17];

                    hexsig(sighex, cl.sig);
                    fprintf(pumpf,
                            "{\"slot\":%" PRIu64 ",\"entry\":%u,\"tx_i\":%u,"
                            "\"sig_hex\":\"%s\",\"version\":%u,\"n_ix\":%u,"
                            "\"ix\":[",
                            batch.slot, batch.tx_entry[n], batch.tx_ord[n],
                            sighex, cl.encoding, cl.n_ix);
                    if (wire_tx_ixs(tb, tl, ixs, WIRE_IX_VIEW_MAX, &nix) == 0) {
                        for (k = 0; k < nix && k < WIRE_IX_VIEW_MAX; k++) {
                            hexn(phex, ixs[k].prog, 32);
                            hexn(dhex, ixs[k].disc, 8);
                            fprintf(pumpf,
                                    "%s{\"i\":%u,\"prog_i\":%u,\"fam\":%u,"
                                    "\"nacc\":%u,\"dlen\":%u,\"prog\":\"%s\","
                                    "\"disc\":\"%s\",\"name\":\"%s\"}",
                                    k ? "," : "", k, ixs[k].prog_i, ixs[k].fam,
                                    ixs[k].nacc, ixs[k].dlen, phex, dhex,
                                    ixs[k].fam == WIRE_FAM_PUMP_AMM
                                            || ixs[k].fam == WIRE_FAM_PUMP_BOND
                                        ? disc_name(ixs[k].disc)
                                        : "n/a");
                        }
                    }
                    fprintf(pumpf, "]}\n");
                }
                {
                    int dcr = ingress_decode_tx(tb, tl, batch.slot, &probe);

                    if (dcr == DEC_ALT) {
                        n_alt++;
                    }
                    if (cl.klass != WIRE_CL_DIRECT_PUMP) {
                        continue;
                    }
                    if (dcr != DEC_OK && dcr != DEC_ALT) {
                        continue;
                    }
                }
                {
                    uint32_t pi, pump_ix = 0;

                    for (pi = 0; pi < probe.n_ix; pi++) {
                        if (probe.ix[pi].proto == PROTO_PUMP
                            && (probe.ix[pi].kind == IX_KIND_PUMP_SELL
                                || probe.ix[pi].kind == IX_KIND_PUMP_BUY_EQ
                                || probe.ix[pi].kind == IX_KIND_PUMP_BUY)) {
                            pump_ix = 1;
                            break;
                        }
                    }
                    if (!pump_ix) {
                        continue;
                    }
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
                if (out.fail == WIRE_FAIL_PRE_VQ || out.fail == WIRE_FAIL_PRE_FEE
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
                    if (n_txpub < SAMP_MAX
                        && out.tsc_publish >= out.tsc_tx_ready) {
                        tx_pub[n_txpub++] = (uint64_t)((double)(out.tsc_publish
                            - out.tsc_tx_ready) * 1e9 / hz);
                    }
                }
                if (out.tx_before_pre) {
                    n_tx_first++;
                }
            }
            pr = shred_assem_next(as, &batch);
        }
    }

    fs = shred_assem_fec_stats(as);
    ss = shred_assem_stream_stats(as);
    printf("WIRE-TO-STATE-003  Reed-Solomon / Merkle FEC recover\n");
    printf("cap=%s  kind=%s  tsc_hz=%.6e  pkts=%u\n", path,
           cap.kind == CAPIO_FEED ? "FEEDCAP1" : "FIRMCAP1", hz, pkts);
    printf("scoreboard:\n");
    printf("  fec_possible=%u  fec_attempted=%u  fec_recovered=%u\n",
           fs != NULL ? fs->possible : 0, fs != NULL ? fs->attempted : 0,
           fs != NULL ? fs->recovered : 0);
    printf("  fec_auth_ok=%u  fec_auth_fail=%u\n",
           fs != NULL ? fs->auth_ok : 0, fs != NULL ? fs->auth_fail : 0);
    printf("  data_sets_completed=%u  one=%u  multi=%u  fec=%u\n",
           n_sets, n_one, n_multi, n_fec);
    printf("  txs_framed=%u  empty_complete=%u  pump=%u  ALT=%u\n",
           n_tx, n_empty, n_pump, n_alt);
    if (ss != NULL) {
        printf("  auth_shreds=%" PRIu64 "  contig_bytes=%" PRIu64
               "  entries=%" PRIu64 "\n",
               ss->auth_shreds, ss->contig_bytes, ss->entries);
        printf("  stream_txs=%" PRIu64 "  legacy=%" PRIu64 "  v0=%" PRIu64
               "  v1=%" PRIu64 "  pump=%" PRIu64 "\n",
               ss->txs, ss->tx_legacy, ss->tx_v0, ss->tx_v1, ss->pump);
        printf("  slot_stream_bytes=%" PRIu64 "  watermark_advances=%" PRIu64
               "  leftover=%" PRIu64 "\n",
               ss->slot_stream_bytes, ss->watermark_advances,
               ss->leftover_bytes);
        printf("  entries_incremental=%" PRIu64 "  sig_ok=%" PRIu64
               "  sig_fail=%" PRIu64 "  frozen=%" PRIu64 "  gap_blocked=%" PRIu64
               "\n",
               ss->entries_incremental, ss->sig_verify_ok, ss->sig_verify_fail,
               ss->frozen, ss->gap_blocked);
    }
    printf("  classify verified=%" PRIu64 "  direct_pump=%" PRIu64
           "  direct_dlmm=%" PRIu64 "\n",
           cs.verified, cs.direct_pump, cs.direct_dlmm);
    printf("  known_router=%" PRIu64 "  watched=%" PRIu64
           "  unresolved_alt=%" PRIu64 "  other=%" PRIu64 "\n",
           cs.known_router, cs.watched, cs.unresolved_alt, cs.other);
    printf("  FAST_CUSTOM candidates=%u  exact=%u  prestate_fallback=%u\n",
           n_pump, n_fast, n_pre);
    printf("  incomplete_shreds=%u  cap_err=%u  tx_before_pre=%u\n",
           shred_assem_incomplete(as), n_bad, n_tx_first);
    if (fs != NULL) {
        uint32_t att = fs->attempted != 0 ? fs->attempted : 1;
        printf("cpu (wall, recover path):\n");
        printf("  recovery_cpu_ns total=%" PRIu64 "  mean/attempt %.1f\n",
               fs->recovery_cpu_ns,
               (double)fs->recovery_cpu_ns / (double)att);
        printf("  auth_cpu_ns total=%" PRIu64 "  mean/attempt %.1f\n",
               fs->auth_cpu_ns, (double)fs->auth_cpu_ns / (double)att);
        printf("  parse_cpu_ns total=%" PRIu64 "\n", fs->parse_cpu_ns);
    }
    printf("\nheadline (capture TSC):\n");
    report("first_rx -> enough shards", rx_en, n_en);
    report("first_rx -> contiguous bytes", rx_co, n_co);
    report("first_rx -> entry complete", rx_ent, n_entc);
    report("first_rx -> data_complete", rx_ds, n_ds);
    report("first_rx -> tx_complete", rx_tx, n_rxtx);
    report("first_rx -> exact publish", rx_pub, n_rxpub);
    report("tx_complete -> exact publish", tx_pub, n_txpub);
    printf("\nFAST_CUSTOM exact needs certified AUTH. Caps are shreds only.\n");
    printf("DLMM blocked.\n");
    if (dump != NULL) {
        fclose(dump);
    }
    if (pumpf != NULL) {
        fclose(pumpf);
    }
    capio_close(&cap);
    shred_assem_free(as);
    free(st);
    free(rx_tx);
    free(rx_pub);
    free(tx_pub);
    free(rx_ds);
    free(rx_en);
    free(rx_co);
    free(rx_ent);
    return 0;
}
