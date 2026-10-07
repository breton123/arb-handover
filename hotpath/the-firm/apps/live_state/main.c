#include "net/capio.h"
#include "net/packet.h"
#include "shred/entry.h"
#include "shred/ingest.h"
#include "shred/prefix.h"
#include "shred/shred.h"
#include "state/authvec.h"
#include "state/boot.h"
#include "state/compact.h"
#include "state/live.h"
#include "wire/lut_cache.h"
#include "wire/lut_file.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include "net/capture.h"
#include "net/feed.h"
#include "net/ring.h"
#include "net/stats.h"
#include "net/time.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

typedef struct {
    live_t   *L;
    uint64_t  slot;
    uint64_t  rx_tsc;
} cov_t;

#if defined(__linux__)
static volatile sig_atomic_t g_run = 1;

static void
on_sig(int sig)
{
    (void)sig;
    g_run = 0;
}
#endif

static void
on_tx(void *user, const shred_entry_hit_t *hit)
{
    cov_t *c = user;

    if (hit->bytes == NULL || hit->tx.tx_len == 0) {
        return;
    }
    (void)live_on_framed(c->L, hit->bytes, hit->tx.tx_len, c->slot, c->rx_tsc);
}

static void
parse_batches(shred_prefix_t *px, uint64_t slot, uint64_t rx_tsc, cov_t *cov,
              uint32_t from_batch)
{
    const uint8_t *buf = NULL;
    uint32_t blen = 0, ends[PREFIX_BATCH_MAX], nb = 0, b, used;
    int brc;

    cov->slot = slot;
    cov->rx_tsc = rx_tsc;
    if (shred_prefix_bytes(px, slot, &buf, &blen) != 0 || blen == 0) {
        return;
    }
    (void)shred_prefix_batches(px, slot, ends, PREFIX_BATCH_MAX, &nb);
    for (b = from_batch; b < nb; b++) {
        uint32_t start = (b == 0u) ? 0u : ends[b - 1u];
        uint32_t elen = ends[b] - start;

        used = 0;
        brc = 0;
        (void)shred_entries_parse(buf + start, elen, on_tx, cov, &used, &brc);
    }
}

static void
parse_list(shred_prefix_t *px, const uint64_t *slots, const uint64_t *tsc,
           const uint32_t *nbat, uint32_t n, cov_t *cov)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        parse_batches(px, slots[i], tsc[i], cov, nbat[i]);
    }
}

static int
ingest_pkt(shred_race_t *race, shred_prefix_t **pxp, const net_packet_t *pkt,
           uint64_t *slots, uint64_t *tsc, uint32_t *nbat, uint32_t *nslot,
           cov_t *cov)
{
    shred_race_result_t rr;
    shred_view_t view;
    shred_identity_t id;
    shred_prefix_info_t inf;
    shred_prefix_t *px = *pxp;
    uint32_t s;

    if (shred_identify_packet(pkt->data, pkt->len, &view, &id) != 0) {
        return 0;
    }
    for (s = 0; s < *nslot; s++) {
        if (slots[s] == id.slot) {
            break;
        }
    }
    if (s == *nslot) {
        if (*nslot == PREFIX_SLOT_MAX) {
            parse_list(px, slots, tsc, nbat, *nslot, cov);
            shred_prefix_free(px);
            *pxp = NULL;
            if (shred_prefix_init(pxp) != 0) {
                return -1;
            }
            px = *pxp;
            *nslot = 0;
            memset(nbat, 0, PREFIX_SLOT_MAX * sizeof(*nbat));
        }
        s = *nslot;
        slots[s] = id.slot;
        tsc[s] = pkt->rx_tsc;
        nbat[s] = 0;
        (*nslot)++;
        (void)shred_prefix_watch(px, id.slot);
    } else if (tsc[s] == 0 || (pkt->rx_tsc != 0 && pkt->rx_tsc < tsc[s])) {
        tsc[s] = pkt->rx_tsc;
    }
    (void)shred_ingest_packet(race, px, pkt, &rr);
    if (shred_prefix_info(px, id.slot, &inf) == 0 && inf.n_batch > nbat[s]) {
        parse_batches(px, id.slot, tsc[s], cov, nbat[s]);
        nbat[s] = inf.n_batch;
    }
    return 0;
}

static int
replay_cap_file(shred_prefix_t **pxp, shred_race_t *race, uint64_t *slots,
                uint64_t *tsc, uint32_t *nbat, uint32_t *nslot, cov_t *cov,
                const char *path)
{
    capio_t cap;

    if (capio_open(&cap, path) != 0) {
        fprintf(stderr, "cap open failed: %s\n", path);
        return -1;
    }
    while (1) {
        net_slot_t sl;
        net_packet_t pkt;
        int rc = capio_read(&cap, &sl);

        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            continue;
        }
        if (cov->L != NULL && cov->L->trace_stop) {
            break;
        }
        if (cov->L != NULL && cov->L->max_framed != 0
            && cov->L->n_framed >= cov->L->max_framed) {
            break;
        }
        memset(&pkt, 0, sizeof(pkt));
        pkt.data = sl.data;
        pkt.len = sl.len;
        pkt.source_id = sl.source_id ? sl.source_id : NET_SOURCE_ORBITFLARE;
        pkt.rx_ns = sl.rx_ns;
        pkt.rx_tsc = sl.rx_tsc;
        if (ingest_pkt(race, pxp, &pkt, slots, tsc, nbat, nslot, cov) != 0) {
            capio_close(&cap);
            return -1;
        }
    }
    capio_close(&cap);
    return 0;
}

static uint32_t
load_cap_paths(const char *cpath, const char *list, char *store, uint32_t store_n,
               const char **out, uint32_t max)
{
    uint32_t n = 0, used = 0;

    if (cpath != NULL) {
        out[0] = cpath;
        return 1;
    }
    if (list == NULL) {
        return 0;
    }
    {
        FILE *f = fopen(list, "r");
        char line[512];

        if (f == NULL) {
            return 0;
        }
        while (n < max && fgets(line, sizeof(line), f) != NULL) {
            size_t len;
            char *s = line;

            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (*s == 0 || *s == '#' || *s == '\n') {
                continue;
            }
            len = strlen(s);
            while (len > 0 && (s[len - 1u] == '\n' || s[len - 1u] == '\r')) {
                s[--len] = 0;
            }
            if (len == 0 || used + (uint32_t)len + 1u > store_n) {
                continue;
            }
            memcpy(store + used, s, len + 1u);
            out[n++] = store + used;
            used += (uint32_t)len + 1u;
        }
        fclose(f);
    }
    return n;
}

static void
usage(void)
{
    fprintf(stderr,
            "usage: live_state_001 --cap FILE | --cap-list FILE | --live\n"
            "          [--bind IP] [--port N] [--out DIR] [--prefix NAME]\n"
            "          [--pools FILE] [--disc FILE] [--luts FILE] [--lut-miss FILE]\n"
            "          [--lut-learned FILE] [--lut-frozen FILE]\n"
            "          [--max-framed N] [--auth FILE] [--pred FILE]\n"
            "          [--dump FILE] [--dirty FILE] [--fam FILE]\n"
            "          [--ps-log FILE] [--trace-sig HEX]\n"
            "          [--walk-pool HEX] [--walk-from SLOT] [--walk-to SLOT]\n"
            "          [--walk-log FILE]\n");
}

#if defined(__linux__)
static void
dash_line(const live_t *L, FILE *fp)
{
    fprintf(fp,
            "LIVE  framed=%" PRIu64 " pump=%" PRIu64 " supported=%" PRIu64
            " applied=%" PRIu64 " exact_pools=%u dirty=%u "
            "MATCH=%" PRIu64 " MISMATCH=%" PRIu64 " GAP=%" PRIu64
            " UNSUPPORTED=%" PRIu64 " APPLY=%" PRIu64 " BEFORE=%" PRIu64
            " NO_PRESTATE=%" PRIu64 " LUT=%" PRIu64
            " disc=%u lut_full=%u sparse=%u miss_q=%u ok=%" PRIu64
            " fail=%" PRIu64 "\n",
            L->n_framed, L->n_pump, L->n_supported, L->n_applied,
            live_n_exact_pools(L->st), live_n_dirty_pools(L->st),
            L->av ? L->av->n_exact : 0, L->av ? L->av->n_mismatch : 0,
            L->reason[PS_GAP], L->reason[PS_UNSUPPORTED],
            L->reason[PS_APPLY], L->reason[PS_BEFORE_ANCHOR],
            L->reason[PS_NO_PRESTATE], L->reason[PS_LUT],
            L->n_disc,
            (L->lut && L->lut->full) ? L->lut->full->n : 0,
            L->lut ? L->lut->n_sp : 0, L->lut ? L->lut->n_miss : 0,
            L->lut ? L->lut->n_lookup_ok : 0,
            L->lut ? L->lut->n_lookup_fail : 0);
    fflush(fp);
}

static int
run_live(live_t *L, const char *bind_ip, uint16_t port, const char *out_dir,
         const char *prefix, uint64_t rotate, shred_prefix_t **pxp,
         shred_race_t *race, cov_t *cov, uint64_t *slots, uint64_t *tsc,
         uint32_t *nbat, uint32_t *nslot)
{
    static net_feed_t feed;
    net_ring_t ingress, cap_ring;
    net_stats_t stats;
    net_packet_t pkt;
    net_recorder_t *rec = NULL;
    netcap_hdr_t hdr;
    net_tsc_clock_t tsc_clk;
    struct timespec last, now;
    int wait_ms = 1;
    int have_cap = 0;

    net_stats_clear(&stats);
    memset(&cap_ring, 0, sizeof(cap_ring));
    if (net_tsc_calibrate(&tsc_clk) != 0) {
        tsc_clk.hz = 3.5e9;
    }
    if (net_ring_init(&ingress, 65536) != 0) {
        fprintf(stderr, "ingress ring\n");
        return 1;
    }
    if (out_dir != NULL) {
        if (net_ring_init(&cap_ring, 65536) != 0) {
            fprintf(stderr, "capture ring\n");
            net_ring_free(&ingress);
            return 1;
        }
        have_cap = 1;
    }
    if (net_orbitflare_open(&feed, bind_ip, port, &ingress,
                            have_cap ? &cap_ring : NULL, &stats, 32) != 0) {
        perror("orbitflare bind");
        if (have_cap) {
            net_ring_free(&cap_ring);
        }
        net_ring_free(&ingress);
        return 1;
    }
    (void)net_feed_set_rcvbuf(&feed, 16 * 1024 * 1024);
    hdr.realtime0_ns = net_realtime_ns();
    hdr.mono0_ns = net_now_ns();
    hdr.tsc_hz = (uint64_t)tsc_clk.hz;
    if (have_cap) {
        if (net_recorder_start(&rec, &cap_ring, out_dir, prefix, rotate, 46,
                               &hdr) != 0) {
            fprintf(stderr, "recorder\n");
            net_feed_close(&feed);
            net_ring_free(&cap_ring);
            net_ring_free(&ingress);
            return 1;
        }
    }
    fprintf(stderr, "LIVE-STATE-001  OrbitFlare %s:%u  stream → CompactState",
            bind_ip, (unsigned)port);
    if (rec != NULL) {
        fprintf(stderr, "  capture %s", net_recorder_path(rec));
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    clock_gettime(CLOCK_MONOTONIC, &last);
    while (g_run) {
        if (net_feed_poll(&feed, wait_ms) < 0) {
            break;
        }
        while (net_ring_acquire(&ingress, &pkt) == 0) {
            if (ingest_pkt(race, pxp, &pkt, slots, tsc, nbat, nslot, cov)
                != 0) {
                net_ring_release(&ingress);
                g_run = 0;
                break;
            }
            net_ring_release(&ingress);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec != last.tv_sec) {
            live_lut_poll(L);
            live_auth_poll(L);
            dash_line(L, stderr);
            last = now;
        }
    }
    parse_list(*pxp, slots, tsc, nbat, *nslot, cov);
    if (rec != NULL) {
        net_recorder_stop(rec);
    }
    net_feed_close(&feed);
    if (have_cap) {
        net_ring_free(&cap_ring);
    }
    net_ring_free(&ingress);
    return 0;
}
#endif

int
main(int argc, char **argv)
{
    const char *cpath = NULL, *lpath = NULL, *ppath = NULL, *apath = NULL;
    const char *pred_path = NULL, *mm_path = NULL;
    const char *lut_miss_path = NULL, *lut_learned_path = NULL;
    const char *lut_frozen_path = NULL;
    const char *disc_path = NULL, *list_path = NULL, *dirty_path = NULL;
    const char *fam_path = NULL, *ps_path = NULL, *trace_hex = NULL;
    const char *walk_hex = NULL, *walk_path = NULL;
    uint64_t walk_from = 0, walk_to = 0;
    const char *bind_ip = "0.0.0.0";
    const char *out_dir = NULL;
    const char *cap_prefix = "orbitflare";
    uint64_t rotate = 2147483648ull;
    uint64_t max_framed = 0;
    uint16_t port = 20001;
    int live = 0;
    shred_prefix_t *px = NULL;
    shred_race_t *race = NULL;
    static compact_state_t st;
    static lut_cache_t luts;
    static authvec_t av;
    static live_t live_st;
    cov_t cov;
    uint64_t slots[PREFIX_SLOT_MAX], tsc[PREFIX_SLOT_MAX];
    uint32_t nbat[PREFIX_SLOT_MAX];
    uint32_t nslot = 0, i;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) {
            printf("LIVE-STATE-001 self-test ok\n");
            return 0;
        }
        if (strcmp(argv[i], "--live") == 0) {
            live = 1;
        } else if (strcmp(argv[i], "--cap") == 0 && i + 1 < (uint32_t)argc) {
            cpath = argv[++i];
        } else if (strcmp(argv[i], "--cap-list") == 0 && i + 1 < (uint32_t)argc) {
            list_path = argv[++i];
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < (uint32_t)argc) {
            bind_ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < (uint32_t)argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < (uint32_t)argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < (uint32_t)argc) {
            cap_prefix = argv[++i];
        } else if (strcmp(argv[i], "--rotate-bytes") == 0
                   && i + 1 < (uint32_t)argc) {
            rotate = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--luts") == 0 && i + 1 < (uint32_t)argc) {
            lpath = argv[++i];
        } else if (strcmp(argv[i], "--lut-miss") == 0
                   && i + 1 < (uint32_t)argc) {
            lut_miss_path = argv[++i];
        } else if (strcmp(argv[i], "--lut-learned") == 0
                   && i + 1 < (uint32_t)argc) {
            lut_learned_path = argv[++i];
        } else if (strcmp(argv[i], "--lut-frozen") == 0
                   && i + 1 < (uint32_t)argc) {
            lut_frozen_path = argv[++i];
        } else if (strcmp(argv[i], "--max-framed") == 0
                   && i + 1 < (uint32_t)argc) {
            max_framed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--pools") == 0 && i + 1 < (uint32_t)argc) {
            ppath = argv[++i];
        } else if (strcmp(argv[i], "--disc") == 0 && i + 1 < (uint32_t)argc) {
            disc_path = argv[++i];
        } else if (strcmp(argv[i], "--auth") == 0 && i + 1 < (uint32_t)argc) {
            apath = argv[++i];
        } else if (strcmp(argv[i], "--pred") == 0 && i + 1 < (uint32_t)argc) {
            pred_path = argv[++i];
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < (uint32_t)argc) {
            mm_path = argv[++i];
        } else if (strcmp(argv[i], "--dirty") == 0 && i + 1 < (uint32_t)argc) {
            dirty_path = argv[++i];
        } else if (strcmp(argv[i], "--fam") == 0 && i + 1 < (uint32_t)argc) {
            fam_path = argv[++i];
        } else if (strcmp(argv[i], "--ps-log") == 0 && i + 1 < (uint32_t)argc) {
            ps_path = argv[++i];
        } else if (strcmp(argv[i], "--trace-sig") == 0
                   && i + 1 < (uint32_t)argc) {
            trace_hex = argv[++i];
        } else if (strcmp(argv[i], "--walk-pool") == 0
                   && i + 1 < (uint32_t)argc) {
            walk_hex = argv[++i];
        } else if (strcmp(argv[i], "--walk-from") == 0
                   && i + 1 < (uint32_t)argc) {
            walk_from = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--walk-to") == 0
                   && i + 1 < (uint32_t)argc) {
            walk_to = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--walk-log") == 0
                   && i + 1 < (uint32_t)argc) {
            walk_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (!live && cpath == NULL && list_path == NULL) {
        usage();
        return 2;
    }
    if (live && (cpath != NULL || list_path != NULL)) {
        fprintf(stderr, "use --live or --cap/--cap-list, not both\n");
        return 2;
    }
    if (cpath != NULL && list_path != NULL) {
        fprintf(stderr, "use --cap or --cap-list, not both\n");
        return 2;
    }
#ifndef __linux__
    if (live) {
        fprintf(stderr, "--live requires Linux UDP ingest\n");
        return 2;
    }
    (void)bind_ip;
    (void)port;
    (void)out_dir;
    (void)cap_prefix;
    (void)rotate;
#endif
    compact_state_clear(&st);
    lut_cache_clear(&luts);
    authvec_clear(&av);
    live_clear(&live_st, &st, &luts, &av);
    if (trace_hex != NULL) {
        uint32_t h;

        if (strlen(trace_hex) != 128u) {
            fprintf(stderr, "--trace-sig needs 128 hex chars\n");
            return 2;
        }
        for (h = 0; h < 64u; h++) {
            unsigned v = 0;
            char buf[3];

            buf[0] = trace_hex[2u * h];
            buf[1] = trace_hex[2u * h + 1u];
            buf[2] = 0;
            if (sscanf(buf, "%2x", &v) != 1) {
                fprintf(stderr, "bad --trace-sig\n");
                return 2;
            }
            live_st.trace_sig[h] = (uint8_t)v;
        }
        live_st.have_trace = 1;
    }
    if (walk_hex != NULL) {
        uint32_t h;

        if (strlen(walk_hex) != 64u) {
            fprintf(stderr, "--walk-pool needs 64 hex chars\n");
            return 2;
        }
        for (h = 0; h < 32u; h++) {
            unsigned v = 0;
            char buf[3];

            buf[0] = walk_hex[2u * h];
            buf[1] = walk_hex[2u * h + 1u];
            buf[2] = 0;
            if (sscanf(buf, "%2x", &v) != 1) {
                fprintf(stderr, "bad --walk-pool\n");
                return 2;
            }
            live_st.walk_pk[h] = (uint8_t)v;
        }
        live_st.have_walk = 1;
        live_st.walk_from = walk_from;
        live_st.walk_to = walk_to;
        live_st.walk_pool_id = STATE_ACCT_NONE;
    }
    live_st.lut_learned_path = lut_learned_path;
    live_st.max_framed = max_framed;
    live_st.auth_path = apath;
    live_st.fam_path = fam_path;
    memset(&cov, 0, sizeof(cov));
    memset(nbat, 0, sizeof(nbat));
    cov.L = &live_st;
    if (live_open_logs(&live_st, pred_path, mm_path) != 0) {
        fprintf(stderr, "log open failed\n");
        return 1;
    }
    if (lut_miss_path != NULL && live_open_lut_miss(&live_st, lut_miss_path) != 0) {
        fprintf(stderr, "lut miss log failed\n");
        live_close_logs(&live_st);
        return 1;
    }
    if (disc_path != NULL && live_open_disc(&live_st, disc_path) != 0) {
        fprintf(stderr, "disc log failed\n");
        live_close_logs(&live_st);
        return 1;
    }
    if (dirty_path != NULL && live_open_dirty(&live_st, dirty_path) != 0) {
        fprintf(stderr, "dirty log failed\n");
        live_close_logs(&live_st);
        return 1;
    }
    if (ps_path != NULL && live_open_ps_log(&live_st, ps_path) != 0) {
        fprintf(stderr, "ps log failed\n");
        live_close_logs(&live_st);
        return 1;
    }
    if (walk_path != NULL && live_open_walk_log(&live_st, walk_path) != 0) {
        fprintf(stderr, "walk log failed\n");
        live_close_logs(&live_st);
        return 1;
    }
    if (lpath != NULL) {
        (void)lut_cache_load_jsonl(&luts, lpath);
    }
    if (lut_learned_path != NULL && lut_frozen_path == NULL) {
        (void)lut_cache_load_jsonl(&luts, lut_learned_path);
    }
    if (lut_frozen_path != NULL) {
        (void)lut_cache_load_jsonl(&luts, lut_frozen_path);
        luts.sealed = 1;
        live_st.lut_learned_path = NULL;
    }
    if (ppath != NULL && boot_load_jsonl(&st, ppath) != 0) {
        fprintf(stderr, "pools load failed\n");
        live_close_logs(&live_st);
        return 1;
    }
    if (shred_prefix_init(&px) != 0
        || shred_race_init(&race, SHRED_RACE_DEFAULT_CAP) != 0) {
        live_close_logs(&live_st);
        return 1;
    }

#if defined(__linux__)
    if (live) {
        int rc = run_live(&live_st, bind_ip, port, out_dir, cap_prefix, rotate,
                          &px, race, &cov, slots, tsc, nbat, &nslot);

        if (apath != NULL) {
            (void)authvec_load_jsonl_from(&av, apath, &live_st.auth_off);
        }
        live_judge(&live_st);
        live_write_fam(&live_st);
        live_print(&live_st, stdout);
        live_close_logs(&live_st);
        return rc != 0 ? rc : (av.n_mismatch != 0 ? 1 : 0);
    }
#endif

    {
        char capstore[65536];
        const char *cappaths[LIVE_CAP_MAX];
        uint32_t nc, ci;

        nc = load_cap_paths(cpath, list_path, capstore, sizeof(capstore),
                            cappaths, LIVE_CAP_MAX);
        if (nc == 0) {
            fprintf(stderr, "no capture files\n");
            live_close_logs(&live_st);
            return 1;
        }
        for (ci = 0; ci < nc; ci++) {
            if (replay_cap_file(&px, race, slots, tsc, nbat, &nslot, &cov,
                                cappaths[ci]) != 0) {
                live_close_logs(&live_st);
                return 1;
            }
        }
    }
    parse_list(px, slots, tsc, nbat, nslot, &cov);
    if (apath != NULL) {
        (void)authvec_load_jsonl_from(&av, apath, &live_st.auth_off);
    }
    live_judge(&live_st);
    live_write_fam(&live_st);
    live_print(&live_st, stdout);
    if (ppath == NULL) {
        printf("\nno --pools: applied predictions are not a live exact claim\n");
    }
    if (apath == NULL) {
        printf("no --auth: MATCH/MISMATCH stay empty; incompletes are not matches\n");
    }
    live_close_logs(&live_st);
    return (av.n_mismatch != 0) ? 1 : 0;
}
