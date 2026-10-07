#include "deps/class.h"
#include "ingress/decode.h"
#include "net/capio.h"
#include "shred/assemble.h"
#include "shred/sig.h"
#include "state/compact.h"
#include "wire/classify.h"
#include "wire/lut_file.h"
#include "wire/pipeline.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define YS_MAX 800000u

#define OUT_NO_LUT                    0u
#define OUT_NO_PRESTATE               1u
#define OUT_TX_MISMATCH               2u
#define OUT_FAST_CUSTOM_FALLBACK      3u
#define OUT_FAST_CUSTOM_EXACT_MATCH   4u
#define OUT_FAST_CUSTOM_EXACT_MISMATCH 5u

typedef struct {
    uint8_t  used;
    uint8_t  version;
    uint8_t  has_index;
    uint8_t  has_post;
    uint8_t  post_eq;
    uint8_t  pamm;
    uint64_t slot;
    uint32_t index;
    uint64_t t_ys_tx;
    uint64_t t_ys_state;
    char     sig[129];
} ys_row_t;

static int
hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
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

static int
pamm_swap(const uint8_t *tx, uint32_t len)
{
    wire_ix_view_t ix[WIRE_IX_VIEW_MAX];
    uint32_t n = 0, i;
    static const uint8_t sell[8] = {
        0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
    };
    static const uint8_t buyeq[8] = {
        0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70
    };
    static const uint8_t buyout[8] = {
        0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
    };

    if (wire_tx_ixs(tx, len, ix, WIRE_IX_VIEW_MAX, &n) != 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (ix[i].fam != WIRE_FAM_PUMP_AMM) {
            continue;
        }
        if (memcmp(ix[i].disc, sell, 8) == 0 || memcmp(ix[i].disc, buyeq, 8) == 0
            || memcmp(ix[i].disc, buyout, 8) == 0) {
            return 1;
        }
    }
    return 0;
}

static uint64_t
json_u64(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    const char *c;

    if (p == NULL) {
        return 0;
    }
    c = strchr(p, ':');
    if (c == NULL) {
        return 0;
    }
    return strtoull(c + 1, NULL, 10);
}

static uint32_t ys_head[65536];
static uint32_t ys_next[YS_MAX];

static uint32_t
ys_hash(const char *s)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < 16u && s[i] != 0; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void
ys_rehash(const ys_row_t *rows, uint32_t n)
{
    uint32_t i;

    memset(ys_head, 0xff, sizeof(ys_head));
    for (i = 0; i < n; i++) {
        uint32_t h = ys_hash(rows[i].sig) & 65535u;

        ys_next[i] = ys_head[h];
        ys_head[h] = i;
    }
}

static int
parse_ys_tx(const char *line, ys_row_t *r)
{
    const char *sig;
    uint32_t i;

    if (strstr(line, "\"ys_tx\"") == NULL) {
        return 0;
    }
    sig = strstr(line, "\"sig_hex\"");
    if (sig == NULL) {
        return 0;
    }
    sig = strchr(sig, ':');
    if (sig == NULL) {
        return 0;
    }
    sig = strchr(sig, '"');
    if (sig == NULL) {
        return 0;
    }
    sig++;
    memset(r, 0, sizeof(*r));
    r->used = 1;
    for (i = 0; i < 128u && hexval(sig[i]) >= 0; i++) {
        char c = sig[i];

        r->sig[i] = (char)((c >= 'A' && c <= 'F') ? c + 32 : c);
    }
    r->sig[128] = 0;
    r->slot = json_u64(line, "\"slot\"");
    if (strstr(line, "\"index\"") != NULL) {
        r->has_index = 1;
        r->index = (uint32_t)json_u64(line, "\"index\"");
    }
    r->t_ys_tx = json_u64(line, "\"t_ys_tx\"");
    r->t_ys_state = json_u64(line, "\"t_ys_state\"");
    r->version = (uint8_t)json_u64(line, "\"version\"");
    if (strstr(line, "\"post_eq\"") != NULL) {
        r->has_post = 1;
        r->post_eq = (uint8_t)(json_u64(line, "\"post_eq\"") != 0);
    }
    if (strstr(line, "\"pamm\":true") != NULL
        || strstr(line, "\"pamm\": true") != NULL) {
        r->pamm = 1;
    }
    return 1;
}

static int
load_ys(const char *path, ys_row_t *rows, uint32_t cap, uint32_t *n)
{
    FILE *f;
    char line[2048];
    uint32_t ntx = 0, skip = 0;

    *n = 0;
    memset(ys_head, 0xff, sizeof(ys_head));
    if (path == NULL) {
        return 0;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "\"ys_tx\"") != NULL) {
            ntx++;
        }
    }
    if (ntx > cap) {
        skip = ntx - cap;
    }
    rewind(f);
    ntx = 0;
    while (fgets(line, sizeof(line), f) != NULL && *n < cap) {
        if (strstr(line, "\"ys_tx\"") == NULL) {
            continue;
        }
        if (ntx < skip) {
            ntx++;
            continue;
        }
        ntx++;
        if (parse_ys_tx(line, &rows[*n])) {
            (*n)++;
        }
    }
    fclose(f);
    ys_rehash(rows, *n);
    return 0;
}

static const ys_row_t *
ys_find(const ys_row_t *rows, uint32_t n, const char *sig)
{
    uint32_t h = ys_hash(sig) & 65535u;
    uint32_t i = ys_head[h];
    const ys_row_t *best = NULL;

    while (i != UINT32_MAX && i < n) {
        if (rows[i].used && strcmp(rows[i].sig, sig) == 0) {
            if (best == NULL || rows[i].pamm) {
                best = &rows[i];
            }
        }
        i = ys_next[i];
    }
    return best;
}

static const char *
out_name(uint8_t o)
{
    switch (o) {
    case OUT_NO_LUT:
        return "NO_LUT";
    case OUT_NO_PRESTATE:
        return "NO_PRESTATE";
    case OUT_TX_MISMATCH:
        return "TX_MISMATCH";
    case OUT_FAST_CUSTOM_FALLBACK:
        return "FAST_CUSTOM_FALLBACK";
    case OUT_FAST_CUSTOM_EXACT_MATCH:
        return "FAST_CUSTOM_EXACT_MATCH";
    case OUT_FAST_CUSTOM_EXACT_MISMATCH:
        return "FAST_CUSTOM_EXACT_MISMATCH";
    default:
        return "UNKNOWN";
    }
}

static int
ys_id_mismatch(const ys_row_t *y, uint64_t slot, uint32_t index)
{
    if (y == NULL) {
        return 0;
    }
    if (y->slot != 0 && y->slot != slot) {
        return 1;
    }
    if (y->has_index && y->index != index) {
        return 1;
    }
    return 0;
}

static uint8_t
classify_hit(const wire_class_t *cl, const ys_row_t *y, uint64_t slot,
             uint32_t index, int wr, const wire_out_t *out)
{
    int exact;

    if (cl->lut_missing || cl->klass == WIRE_CL_UNRESOLVED_ALT
        || (wr == 0 && out->fail == WIRE_FAIL_ALT)) {
        return OUT_NO_LUT;
    }
    if (ys_id_mismatch(y, slot, index)) {
        return OUT_TX_MISMATCH;
    }
    if (wr != 0) {
        return OUT_FAST_CUSTOM_FALLBACK;
    }
    if (out->fail == WIRE_FAIL_PRE_VQ || out->fail == WIRE_FAIL_PRE_FEE
        || out->fail == WIRE_FAIL_PRE_VAULT || out->fail == WIRE_FAIL_PRE_MINT
        || out->fail == WIRE_FAIL_RESOLVE) {
        return OUT_NO_PRESTATE;
    }
    exact = (out->fail == WIRE_FAIL_NONE && out->admit == DEP_FAST_CUSTOM
             && out->apply_rc == APPLY_OK);
    if (exact) {
        if (y != NULL && y->has_post && !y->post_eq) {
            return OUT_FAST_CUSTOM_EXACT_MISMATCH;
        }
        return OUT_FAST_CUSTOM_EXACT_MATCH;
    }
    return OUT_FAST_CUSTOM_FALLBACK;
}

static uint64_t
of_wall_ns(uint64_t origin_ns, uint64_t origin_tsc, uint64_t tsc, double hz)
{
    if (hz <= 0.0 || tsc < origin_tsc) {
        return origin_ns;
    }
    return origin_ns + (uint64_t)((double)(tsc - origin_tsc) * 1e9 / hz);
}

int
main(int argc, char **argv)
{
    capio_t cap;
    shred_assem_t *as = NULL;
    compact_state_t *st;
    wire_lut_tab_t luts;
    ys_row_t *ys = NULL;
    const char *cpath = NULL, *yspath = NULL, *lutpath = NULL, *cands = NULL;
    FILE *cf = NULL;
    uint32_t nys = 0, i, pkts = 0;
    uint64_t n_ver = 0, n_pamm = 0, n_ys = 0, n_ys_slot = 0, n_ys_idx = 0;
    uint64_t n_pre = 0, n_fast = 0, n_bond = 0;
    uint64_t n_out[6] = { 0 };
    uint64_t origin_ns = 0, origin_tsc = 0;
    int self = 0, have_origin = 0, follow = 0, idle = 0, stale = 0;
    long last_pos = -1;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--cap") == 0 && i + 1 < (uint32_t)argc) {
            cpath = argv[++i];
        } else if (strcmp(argv[i], "--ys") == 0 && i + 1 < (uint32_t)argc) {
            yspath = argv[++i];
        } else if (strcmp(argv[i], "--luts") == 0 && i + 1 < (uint32_t)argc) {
            lutpath = argv[++i];
        } else if (strcmp(argv[i], "--cands") == 0 && i + 1 < (uint32_t)argc) {
            cands = argv[++i];
        } else if (strcmp(argv[i], "--follow") == 0) {
            follow = 1;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            self = 1;
        }
    }
    if (self) {
        printf("LIVE-TRUTH-001 self-test ok (no cap)\n");
        return 0;
    }
    if (cpath == NULL) {
        fprintf(stderr,
                "usage: live_truth --cap FILE [--ys FILE] [--luts FILE] "
                "[--cands FILE] [--follow]\n");
        return 2;
    }
    ys = calloc(YS_MAX, sizeof(*ys));
    st = calloc(1, sizeof(*st));
    if (ys == NULL || st == NULL || shred_assem_init(&as) != 0) {
        return 1;
    }
    compact_state_clear(st);
    wire_lut_tab_clear(&luts);
    if (lutpath != NULL && wire_lut_load_jsonl(&luts, lutpath) != 0) {
        fprintf(stderr, "lut cache empty/missing: %s\n", lutpath);
    }
    if (yspath != NULL && load_ys(yspath, ys, YS_MAX, &nys) != 0) {
        fprintf(stderr, "ys load failed: %s\n", yspath);
        return 1;
    }
    if (cands != NULL) {
        cf = fopen(cands, "a");
        if (cf == NULL) {
            fprintf(stderr, "cands open failed: %s\n", cands);
            return 1;
        }
    }
    if (capio_open(&cap, cpath) != 0) {
        fprintf(stderr, "cap open failed: %s\n", cpath);
        return 1;
    }
    while (1) {
        net_slot_t slot;
        shred_batch_t batch;
        int pr, rc;

        rc = capio_read(&cap, &slot);
        if (rc == 1) {
            if (!follow || cap.f == NULL) {
                break;
            }
            clearerr(cap.f);
            idle++;
            if (yspath != NULL && (idle % 30) == 0) {
                nys = 0;
                (void)load_ys(yspath, ys, YS_MAX, &nys);
            }
            if (cap.f != NULL) {
                long pos = ftell(cap.f);

                if (pos >= 0 && pos == last_pos) {
                    stale++;
                } else {
                    stale = 0;
                    last_pos = pos;
                }
                if (stale >= 20) {
                    break;
                }
            }
#ifndef _WIN32
            sleep(1);
#endif
            continue;
        }
        if (rc != 0) {
            continue;
        }
        pkts++;
        if (!have_origin) {
            origin_ns = slot.rx_ns;
            origin_tsc = slot.rx_tsc;
            have_origin = 1;
        }
        if (pkts == 1u) {
            printf("LIVE-TRUTH start ys_rows=%u lut=%u follow=%d\n", nys,
                   luts.n, follow);
            fflush(stdout);
        }
        pr = shred_assem_push(as, slot.data, slot.len, slot.rx_tsc, &batch);
        while (pr == 1) {
            uint32_t t;

            for (t = 0; t < batch.n_tx; t++) {
                const uint8_t *tb = batch.buf + batch.tx_off[t];
                uint32_t tl = batch.tx_len[t];
                wire_class_t cl;
                char sig[129];
                const ys_row_t *y;

                if (wire_classify(tb, tl, &luts, &cl) != 0) {
                    continue;
                }
                if (shred_tx_sig_verify(tb, tl) != 1) {
                    continue;
                }
                n_ver++;
                hexsig(sig, cl.sig);
                if (cl.klass == WIRE_CL_DIRECT_PUMP && !pamm_swap(tb, tl)) {
                    n_bond++;
                }
                y = ys_find(ys, nys, sig);
                if (y != NULL) {
                    n_ys++;
                    if (y->slot == 0 || y->slot == batch.slot) {
                        n_ys_slot++;
                    }
                    if (!y->has_index || y->index == batch.tx_ord[t]) {
                        n_ys_idx++;
                    }
                }
                if (pamm_swap(tb, tl) || (y != NULL && y->pamm)) {
                    wire_in_t in;
                    wire_out_t out;
                    double hz = (cap.tsc_hz != 0) ? (double)cap.tsc_hz : 3.5e9;
                    uint8_t oc;
                    int wr;
                    uint64_t t_first_ns, t_pub_ns, t_ys_state;

                    n_pamm++;
                    memset(&in, 0, sizeof(in));
                    memset(&out, 0, sizeof(out));
                    in.framed = tb;
                    in.framed_len = tl;
                    in.tsc_rx = batch.tsc_first;
                    in.tsc_tx_ready = batch.tsc_done;
                    in.tsc_pre_ready = 0;
                    in.st = st;
                    wr = wire_to_state(&in, &out, hz);
                    if (wr == 0 && out.pre_complete) {
                        n_pre++;
                    }
                    if (wr == 0 && out.fail == WIRE_FAIL_NONE
                        && out.admit == DEP_FAST_CUSTOM) {
                        n_fast++;
                    }
                    oc = classify_hit(&cl, y, batch.slot, batch.tx_ord[t], wr,
                                      &out);
                    n_out[oc]++;
                    fflush(stdout);
                    t_first_ns = of_wall_ns(origin_ns, origin_tsc,
                                            batch.tsc_first, hz);
                    t_pub_ns = of_wall_ns(origin_ns, origin_tsc,
                                          out.tsc_publish, hz);
                    t_ys_state = (y != NULL) ? y->t_ys_state : 0;
                    printf("pAMM %s slot=%" PRIu64 " tx_index=%u sig=%s "
                           "version=%u of_to_local_ns=%" PRIu64
                           " of_to_ys_state_ns=%" PRIu64 "\n",
                           out_name(oc), batch.slot, batch.tx_ord[t], sig,
                           (unsigned)cl.encoding,
                           (out.tsc_publish != 0 && t_pub_ns >= t_first_ns)
                               ? (t_pub_ns - t_first_ns)
                               : 0,
                           (t_ys_state != 0 && t_ys_state >= t_first_ns)
                               ? (t_ys_state - t_first_ns)
                               : 0);
                    if (cf != NULL) {
                        fprintf(cf,
                                "{\"outcome\":\"%s\",\"slot\":%" PRIu64
                                ",\"tx_index\":%u,\"sig\":\"%s\","
                                "\"version\":%u,"
                                "\"t_first_packet\":%" PRIu64
                                ",\"t_prefix_ready\":%" PRIu64
                                ",\"t_tx_complete\":%" PRIu64
                                ",\"t_prestate_ready\":%" PRIu64
                                ",\"t_fast_custom_start\":%" PRIu64
                                ",\"t_publish\":%" PRIu64
                                ",\"t_ys_tx\":%" PRIu64
                                ",\"t_ys_state\":%" PRIu64
                                ",\"of_to_local_ns\":%" PRIu64
                                ",\"of_to_ys_state_ns\":%" PRIu64
                                ",\"fail\":\"%s\"}\n",
                                out_name(oc), batch.slot, batch.tx_ord[t],
                                sig, (unsigned)cl.encoding, batch.tsc_first,
                                batch.tsc_contig, batch.tsc_done,
                                out.tsc_pre_ready, out.tsc_pre_ready,
                                out.tsc_publish,
                                y != NULL ? y->t_ys_tx : 0,
                                t_ys_state,
                                (out.tsc_publish != 0
                                 && t_pub_ns >= t_first_ns)
                                    ? (t_pub_ns - t_first_ns)
                                    : 0,
                                (t_ys_state != 0 && t_ys_state >= t_first_ns)
                                    ? (t_ys_state - t_first_ns)
                                    : 0,
                                wr == 0 ? wire_fail_name(out.fail) : "wire");
                        fflush(cf);
                    }
                }
            }
            pr = shred_assem_next(as, &batch);
        }
    }
    printf("LIVE-TRUTH-001\n");
    printf("cap=%s  ys=%s  luts=%u  pkts=%u\n", cpath,
           yspath != NULL ? yspath : "(none)", luts.n, pkts);
    printf("  verified_txs=%" PRIu64 "\n", n_ver);
    printf("  pamm_swaps=%" PRIu64 "  bonding_family=%" PRIu64 "\n", n_pamm,
           n_bond);
    printf("  ys_sig_hit=%" PRIu64 "  ys_slot_ok=%" PRIu64 "  ys_index_ok=%" PRIu64
           "  ys_rows=%u\n",
           n_ys, n_ys_slot, n_ys_idx, nys);
    printf("  certified_prestate=%" PRIu64 "  FAST_CUSTOM_exact=%" PRIu64 "\n",
           n_pre, n_fast);
    printf("  NO_LUT=%" PRIu64 "  NO_PRESTATE=%" PRIu64 "  TX_MISMATCH=%" PRIu64
           "\n",
           n_out[OUT_NO_LUT], n_out[OUT_NO_PRESTATE],
           n_out[OUT_TX_MISMATCH]);
    printf("  FAST_CUSTOM_FALLBACK=%" PRIu64 "  EXACT_MATCH=%" PRIu64
           "  EXACT_MISMATCH=%" PRIu64 "\n",
           n_out[OUT_FAST_CUSTOM_FALLBACK],
           n_out[OUT_FAST_CUSTOM_EXACT_MATCH],
           n_out[OUT_FAST_CUSTOM_EXACT_MISMATCH]);
    printf("  SHRED_EXACT=no (slot stream is numeric-slot only)\n");
    printf("  DLMM blocked. Bonding not admitted.\n");
    if (cf != NULL) {
        fclose(cf);
    }
    capio_close(&cap);
    shred_assem_free(as);
    free(st);
    free(ys);
    return 0;
}
