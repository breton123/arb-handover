/*
 * PAPER-LIVE-001 — production-shaped paper searcher on raw FEEDCAP1.
 * Real six-venue snapshot. Does not write the .cap. Does not send the arb.
 */
#include "classify_n.h"
#include "feed.h"
#include "hot.h"
#include "live.h"
#include "nonce.h"
#include "paper.h"
#include "pool.h"
#include "proto.h"
#include "route0_v0.h"
#include "tx_template.h"
#include "shred.h"
#include "sign.h"
#include "swapix.h"
#include "syncrec.h"
#include "tsc.h"
#include "tx_n.h"
#include "util.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JRN_MAGIC  0x50313031u /* "P101" */
#define LAT_MAX    (1u << 20)

typedef struct {
    uint64_t rx_ns;
    uint64_t act_ns;
    uint64_t sign_ns;
    uint64_t amount_in;
    uint64_t gross;
    uint32_t slot;
    uint32_t pool_idx;
    uint32_t route_id;
    uint8_t  family;
    uint8_t  n_hop;
    uint8_t  proto[3];
    uint8_t  searchable;
    uint8_t  signed_ready;
    uint8_t  reason;
    uint8_t  pad[4];
} paper_rec_t;

typedef struct {
    uint64_t shred;
    uint64_t bad;
    uint64_t relevant;
    uint64_t framed;
    uint64_t decoded;
    uint64_t dup;
    uint64_t known;
    uint64_t state_have;
    uint64_t state_ok;
    uint64_t routes_eval;
    uint64_t opp;
    uint64_t signed_ready;
    uint64_t exec_missing;
    uint64_t stale;
    uint64_t signer_busy;
    uint64_t proto_hit[PROTO_N];
    uint64_t cut_n[5];
    uint64_t cut_gp[5];
    uint64_t search_gp;
    uint64_t land_gp;
    uint32_t *act;
    uint32_t n_act;
    uint32_t *sgn;
    uint32_t n_sgn;
    uint32_t burst_max;
    uint32_t burst_cur;
    uint64_t burst_win;
} paper_acc_t;

static uint64_t
cyc_ns(const struct tsc_clock *c, uint64_t cyc)
{
    if (c == NULL || c->hz <= 0.0) {
        return 0;
    }
    return (uint64_t)((double)cyc * 1e9 / c->hz);
}

static int
cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t
pct_u32(uint32_t *v, uint32_t n, double p)
{
    uint32_t i;
    if (n == 0) {
        return 0;
    }
    qsort(v, n, sizeof(*v), cmp_u32);
    i = (uint32_t)(p * (double)(n - 1u));
    return v[i];
}

static void
pace(uint64_t first_rx, uint64_t first_mono, uint64_t rx_ns)
{
    struct timespec now;
    uint64_t target;
    uint64_t cur;
    int64_t dt;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    cur = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    target = first_mono + (rx_ns - first_rx);
    dt = (int64_t)target - (int64_t)cur;
    if (dt > 1000) {
        struct timespec sl;
        sl.tv_sec = (time_t)(dt / 1000000000ll);
        sl.tv_nsec = (long)(dt % 1000000000ll);
        (void)clock_nanosleep(CLOCK_MONOTONIC, 0, &sl, NULL);
    }
}

typedef struct {
    uint8_t  sig[64];
    uint64_t rx_ns;
    uint32_t slot;
    uint8_t  used;
    uint8_t  seen;
} win_ent_t;

typedef struct {
    win_ent_t *tab;
    uint32_t   cap;
    uint32_t   n;
    uint32_t   found;
} win_tab_t;

static uint32_t
sig_hash(const uint8_t *s)
{
    uint64_t x;
    memcpy(&x, s, 8);
    return (uint32_t)(x ^ (x >> 32));
}

static int
win_load(win_tab_t *w, const char *path)
{
    FILE *f;
    uint8_t sig[64];
    uint32_t cap;
    uint32_t n = 0;

    memset(w, 0, sizeof(*w));
    if (path == NULL) {
        return 0;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    cap = 1u << 18;
    w->tab = calloc(cap, sizeof(*w->tab));
    if (w->tab == NULL) {
        fclose(f);
        return -1;
    }
    w->cap = cap;
    while (fread(sig, 64, 1, f) == 1) {
        uint32_t mask = cap - 1u;
        uint32_t i = sig_hash(sig) & mask;
        while (w->tab[i].used) {
            if (memcmp(w->tab[i].sig, sig, 64) == 0) {
                break;
            }
            i = (i + 1u) & mask;
        }
        if (!w->tab[i].used) {
            memcpy(w->tab[i].sig, sig, 64);
            w->tab[i].used = 1;
            n++;
        }
    }
    fclose(f);
    w->n = n;
    return 0;
}

static void
win_note(win_tab_t *w, const uint8_t *p, uint16_t len, uint64_t rx, uint32_t slot)
{
    uint16_t i;
    uint32_t mask;

    if (w == NULL || w->tab == NULL || p == NULL || len < 64) {
        return;
    }
    mask = w->cap - 1u;
    for (i = 0; i + 64u <= len; i++) {
        uint32_t h = sig_hash(p + i) & mask;
        uint32_t k = h;
        for (;;) {
            if (!w->tab[k].used) {
                break;
            }
            if (memcmp(w->tab[k].sig, p + i, 64) == 0) {
                if (!w->tab[k].seen) {
                    w->tab[k].seen = 1;
                    w->tab[k].rx_ns = rx;
                    w->tab[k].slot = slot;
                    w->found++;
                }
                return;
            }
            k = (k + 1u) & mask;
            if (k == h) {
                return;
            }
        }
    }
}

static int
walk_file(paper_acc_t *a, live_univ_t *u, univ_state_t *st, win_tab_t *wins,
          route0_v0_template_t *v0, route0_signer_t *signer,
          const struct tsc_clock *clk, FILE *jrn, syncrec_t *sync,
          hot_seen_t *seen, int paced,
          uint64_t *first_rx, uint64_t *first_mono, const char *path)
{
    FILE *f;
    feedcap_hdr_t hdr;
    feed_slot_t slot;
    long sz;
    uint8_t tx[V0_TX_LEN];
    uint8_t bh[32];
    opportunity_t dummy;

    memset(bh, 0xBB, sizeof(bh));
    memset(&dummy, 0, sizeof(dummy));
    dummy.valid = 1;
    dummy.route_id = ROUTE_DLMM_PUMP;
    dummy.amount_in = 10000;
    dummy.direction = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < (long)(FEEDCAP_HDR_LEN + (1u << 20))) {
        fclose(f);
        return 0;
    }
    rewind(f);
    if (feedcap_read_header(f, &hdr) != 0) {
        fclose(f);
        return -1;
    }
    (void)hdr;
    fprintf(stderr, "read  %s\n", path);
    for (;;) {
        shred_view_t v;
        uint16_t plen;
        uint32_t rel;
        int rc = feedcap_read_slot(f, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fclose(f);
            return -1;
        }
        if (paced) {
            struct timespec now;
            if (*first_rx == 0) {
                *first_rx = slot.rx_ns;
                if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                    *first_mono = (uint64_t)now.tv_sec * 1000000000ull
                        + (uint64_t)now.tv_nsec;
                }
            }
            pace(*first_rx, *first_mono, slot.rx_ns);
        }
        if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
            a->bad++;
            continue;
        }
        a->shred++;
        plen = (uint16_t)(slot.len - (uint16_t)(v.payload - slot.data));
        rel = classify_n(v.payload, plen, 6);
        if (rel == REL_N_NONE) {
            continue;
        }
        a->relevant++;
        {
            size_t poff;
            uint8_t pfound;
            if (find_prog_id_n(v.payload, plen, 6, &poff, &pfound) == 0) {
                a->framed++;
            }
        }
        win_note(wins, v.payload, plen, slot.rx_ns, (uint32_t)v.slot);
        {
            affected_pool_t hit;
            hot_trigger_t trig;
            paper_opp_t opp;
            swapix_t nix;
            uint64_t t0 = rdtscp();
            uint64_t t1;
            if (swapix_from_payload(v.payload, plen, &nix) != 0
                || swapix_to_trigger(&nix, &trig) != 0) {
                continue;
            }
            a->decoded++;
            if (hot_seen_first(seen, nix.sig) != 1) {
                a->dup++;
                continue;
            }
            if (pool_table_get(&u->keys, nix.pool, &hit) != 0) {
                continue;
            }
            a->known++;
            {
                uint8_t pr = u->meta[hit.pool_idx].protocol;
                if (pr == PROTO_DLMM
                    || (pr == PROTO_PUMP && u->pump_live[hit.pool_idx])
                    || u->extra_live[hit.pool_idx]) {
                    a->state_have++;
                }
            }
            if (hit.protocol < PROTO_N) {
                a->proto_hit[hit.protocol]++;
            }
            if (paper_eval_trigger(u, st, hit.pool_idx, &trig, &opp) != 0) {
                continue;
            }
            if (sync != NULL) {
                (void)syncrec_decision(sync, slot.rx_ns, &nix, hit.pool_idx,
                                       u->state_version, nix.amount_in,
                                       nix.min_out, &opp.opp);
            }
            a->routes_eval++;
            t1 = rdtscp();
            if (a->n_act < LAT_MAX) {
                a->act[a->n_act++] = (uint32_t)cyc_ns(clk, t1 - t0);
            }
            if (slot.rx_ns / 1000000ull != a->burst_win) {
                if (a->burst_cur > a->burst_max) {
                    a->burst_max = a->burst_cur;
                }
                a->burst_cur = 0;
                a->burst_win = slot.rx_ns / 1000000ull;
            }
            a->burst_cur++;
            if (!opp.opp.valid) {
                continue;
            }
            if (opp.state_ok) {
                a->state_ok++;
            }
            a->opp++;
            a->search_gp += opp.opp.gross_profit;
            if (opp.signed_ready) {
                a->signed_ready++;
                a->land_gp += opp.opp.gross_profit;
            } else {
                a->exec_missing++;
            }
            {
                uint64_t usd_cents = opp.opp.gross_profit * 115ull / 10000000ull;
                uint32_t b;
                for (b = 0; b < 5; b++) {
                    static const uint64_t cut[5] = { 10, 100, 1000, 5000, 10000 };
                    if (usd_cents >= cut[b]) {
                        a->cut_n[b]++;
                        a->cut_gp[b] += opp.opp.gross_profit;
                    }
                }
            }
            if (opp.signed_ready) {
                uint64_t ts = rdtscp();
                if (route0_v0_patch(v0, &dummy, 1, bh, 1, tx) == 0
                    && route0_v0_sign(signer, tx) == 0) {
                    if (a->n_sgn < LAT_MAX) {
                        a->sgn[a->n_sgn++] = (uint32_t)cyc_ns(clk, rdtscp() - t0);
                    }
                } else {
                    a->signer_busy++;
                }
                (void)ts;
            }
            if (jrn != NULL) {
                paper_rec_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.rx_ns = slot.rx_ns;
                rec.act_ns = cyc_ns(clk, t1 - t0);
                rec.sign_ns = (opp.signed_ready && a->n_sgn > 0)
                    ? a->sgn[a->n_sgn - 1] : 0;
                rec.amount_in = opp.opp.amount_in;
                rec.gross = opp.opp.gross_profit;
                rec.slot = (uint32_t)v.slot;
                rec.pool_idx = hit.pool_idx;
                rec.route_id = opp.route_id;
                rec.family = opp.family;
                rec.n_hop = opp.n_hop;
                memcpy(rec.proto, opp.proto, 3);
                rec.searchable = opp.searchable;
                rec.signed_ready = opp.signed_ready;
                rec.reason = opp.reason;
                (void)fwrite(&rec, sizeof(rec), 1, jrn);
            }
        }
    }
    fclose(f);
    return 0;
}

static int
walk_dir(paper_acc_t *a, live_univ_t *u, univ_state_t *st, win_tab_t *wins,
         route0_v0_template_t *v0, route0_signer_t *signer,
         const struct tsc_clock *clk, FILE *jrn, syncrec_t *sync,
         hot_seen_t *seen, int paced, const char *dir)
{
    DIR *dp;
    struct dirent *de;
    char **names = NULL;
    uint32_t n = 0;
    uint32_t i;
    uint64_t first_rx = 0;
    uint64_t first_mono = 0;

    dp = opendir(dir);
    if (dp == NULL) {
        return -1;
    }
    while ((de = readdir(dp)) != NULL) {
        size_t L = strlen(de->d_name);
        char *dup;
        if (L < 5 || strcmp(de->d_name + L - 4, ".cap") != 0) {
            continue;
        }
        dup = malloc(L + 1);
        if (dup == NULL) {
            closedir(dp);
            return -1;
        }
        memcpy(dup, de->d_name, L + 1);
        names = realloc(names, (n + 1u) * sizeof(*names));
        if (names == NULL) {
            free(dup);
            closedir(dp);
            return -1;
        }
        names[n++] = dup;
    }
    closedir(dp);
    for (i = 0; i < n; i++) {
        uint32_t j;
        for (j = i + 1; j < n; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    for (i = 0; i < n; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (walk_file(a, u, st, wins, v0, signer, clk, jrn, sync, seen, paced,
                      &first_rx, &first_mono, path) != 0) {
            fprintf(stderr, "fail  %s\n", path);
        }
        free(names[i]);
    }
    free(names);
    return 0;
}

int
main(int argc, char **argv)
{
    live_univ_t u;
    univ_state_t st;
    route0_v0_template_t v0;
    route0_signer_t signer;
    paper_acc_t acc;
    struct tsc_clock clk;
    win_tab_t wins;
    const char *dir = NULL;
    const char *univ = NULL;
    const char *jpath = NULL;
    const char *wpath = NULL;
    const char *spath = NULL;
    const char *sync_path = NULL;
    syncrec_t sync;
    hot_seen_t seen;
    int paced = 0;
    uint8_t seed[32];
    uint8_t sk[V0_N_STATIC * 32];
    uint8_t wr[V0_N_ALT_WR * 32];
    uint8_t ro[V0_N_ALT_RO * 32];
    uint8_t alt[32];
    FILE *jrn = NULL;
    int ai;

    memset(&acc, 0, sizeof(acc));
    memset(&u, 0, sizeof(u));
    memset(&st, 0, sizeof(st));
    memset(&wins, 0, sizeof(wins));
    memset(&sync, 0, sizeof(sync));
    memset(&seen, 0, sizeof(seen));
    memset(seed, 0x44, 32);
    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--dir") == 0 && ai + 1 < argc) {
            dir = argv[++ai];
        } else if (strcmp(argv[ai], "--univ") == 0 && ai + 1 < argc) {
            univ = argv[++ai];
        } else if (strcmp(argv[ai], "--journal") == 0 && ai + 1 < argc) {
            jpath = argv[++ai];
        } else if (strcmp(argv[ai], "--winners") == 0 && ai + 1 < argc) {
            wpath = argv[++ai];
        } else if (strcmp(argv[ai], "--stats") == 0 && ai + 1 < argc) {
            spath = argv[++ai];
        } else if (strcmp(argv[ai], "--sync") == 0 && ai + 1 < argc) {
            sync_path = argv[++ai];
        } else if (strcmp(argv[ai], "--paced") == 0) {
            paced = 1;
        } else {
            fprintf(stderr, "usage: %s --dir DIR --univ liveuniv.bin [--journal out.bin] [--sync sync.bin] [--winners sigs.bin] [--stats stats.json] [--paced]\n",
                    argv[0]);
            return 1;
        }
    }
    if (dir == NULL || univ == NULL) {
        fprintf(stderr, "usage: %s --dir DIR --univ liveuniv.bin [--journal out.bin] [--winners sigs.bin] [--stats stats.json] [--paced]\n",
                argv[0]);
        return 1;
    }
    acc.act = calloc(LAT_MAX, sizeof(*acc.act));
    acc.sgn = calloc(LAT_MAX, sizeof(*acc.sgn));
    if (acc.act == NULL || acc.sgn == NULL) {
        return 1;
    }
    if (tsc_calibrate(&clk) != 0) {
        fprintf(stderr, "tsc calibrate failed\n");
        return 1;
    }
    if (live_univ_init(&u) != 0 || live_univ_load(&u, univ) != 0) {
        fprintf(stderr, "liveuniv load failed\n");
        return 1;
    }
    if (univ_state_init(&st, u.n) != 0 || live_fill_univ_state(&u, &st) != 0) {
        fprintf(stderr, "univ_state failed\n");
        return 1;
    }
    memset(sk, 0x51, sizeof(sk));
    memset(wr, 0x52, sizeof(wr));
    memset(ro, 0x53, sizeof(ro));
    memset(alt, 0xA1, sizeof(alt));
    if (route0_v0_compile(sk, alt, wr, ro, ROUTE0_CU_LIMIT, &v0) != 0
        || route0_signer_init(&signer, seed) != 0) {
        fprintf(stderr, "v0 template/signer failed\n");
        return 1;
    }
    if (jpath != NULL) {
        uint32_t mag = JRN_MAGIC;
        jrn = fopen(jpath, "wb");
        if (jrn == NULL) {
            return 1;
        }
        fwrite(&mag, 4, 1, jrn);
    }
    if (win_load(&wins, wpath) != 0) {
        fprintf(stderr, "winner sig load failed\n");
        return 1;
    }
    if (hot_seen_init(&seen) != 0) {
        fprintf(stderr, "seen table failed\n");
        return 1;
    }
    if (sync_path != NULL) {
        if (syncrec_open(&sync, sync_path, (uint64_t)clk.hz) != 0
            || syncrec_state_init(&sync, &u, 0) != 0) {
            fprintf(stderr, "sync open failed\n");
            return 1;
        }
    }
    printf("PAPER-LIVE-001  ver=%u pools=%u dlmm=%u pump=%u clmm=%u cpmm=%u damm=%u orca=%u routes=%u winners=%u paced=%d\n",
           u.file_ver, u.n, u.n_dlmm, u.n_pump, u.n_clmm, u.n_cpmm, u.n_damm,
           u.n_orca, u.routes.n_route, wins.n, paced);
    if (walk_dir(&acc, &u, &st, &wins, &v0, &signer, &clk, jrn,
                 sync_path != NULL ? &sync : NULL, &seen, paced, dir) != 0) {
        return 1;
    }
    if (acc.burst_cur > acc.burst_max) {
        acc.burst_max = acc.burst_cur;
    }
    printf("shreds             %" PRIu64 "\n", acc.shred);
    printf("relevant           %" PRIu64 "\n", acc.relevant);
    printf("framed             %" PRIu64 "\n", acc.framed);
    printf("decoded_n          %" PRIu64 "\n", acc.decoded);
    printf("dup_sig            %" PRIu64 "\n", acc.dup);
    printf("known_pool         %" PRIu64 "\n", acc.known);
    printf("state_have         %" PRIu64 "\n", acc.state_have);
    printf("state_ok           %" PRIu64 "\n", acc.state_ok);
    printf("routes_eval        %" PRIu64 "\n", acc.routes_eval);
    printf("opp                %" PRIu64 "\n", acc.opp);
    printf("signed_ready       %" PRIu64 "\n", acc.signed_ready);
    printf("exec_missing       %" PRIu64 "\n", acc.exec_missing);
    printf("search_gp_lamports %" PRIu64 "\n", acc.search_gp);
    printf("land_gp_lamports   %" PRIu64 "\n", acc.land_gp);
    printf("burst_max_1ms      %u\n", acc.burst_max);
    printf("actionable->dec    n=%u p50=%u p99=%u p999=%u ns\n",
           acc.n_act, pct_u32(acc.act, acc.n_act, 0.50),
           pct_u32(acc.act, acc.n_act, 0.99),
           pct_u32(acc.act, acc.n_act, 0.999));
    printf("actionable->sign   n=%u p50=%u p99=%u p999=%u ns\n",
           acc.n_sgn, pct_u32(acc.sgn, acc.n_sgn, 0.50),
           pct_u32(acc.sgn, acc.n_sgn, 0.99),
           pct_u32(acc.sgn, acc.n_sgn, 0.999));
    printf("winner_sigs_seen   %u / %u\n", wins.found, wins.n);
    if (spath != NULL) {
        FILE *sf = fopen(spath, "w");
        if (sf != NULL) {
            fprintf(sf, "{\n");
            fprintf(sf, "  \"shreds\": %" PRIu64 ",\n", acc.shred);
            fprintf(sf, "  \"relevant\": %" PRIu64 ",\n", acc.relevant);
            fprintf(sf, "  \"complete_triggers\": %" PRIu64 ",\n", acc.framed);
            fprintf(sf, "  \"decoded_n\": %" PRIu64 ",\n", acc.decoded);
            fprintf(sf, "  \"dup_sig\": %" PRIu64 ",\n", acc.dup);
            fprintf(sf, "  \"known_pool\": %" PRIu64 ",\n", acc.known);
            fprintf(sf, "  \"state_have\": %" PRIu64 ",\n", acc.state_have);
            fprintf(sf, "  \"state_sufficient\": %" PRIu64 ",\n", acc.state_ok);
            fprintf(sf, "  \"routes_eval\": %" PRIu64 ",\n", acc.routes_eval);
            fprintf(sf, "  \"opp\": %" PRIu64 ",\n", acc.opp);
            fprintf(sf, "  \"signed_ready\": %" PRIu64 ",\n", acc.signed_ready);
            fprintf(sf, "  \"exec_missing\": %" PRIu64 ",\n", acc.exec_missing);
            fprintf(sf, "  \"search_gp_lamports\": %" PRIu64 ",\n", acc.search_gp);
            fprintf(sf, "  \"land_gp_lamports\": %" PRIu64 ",\n", acc.land_gp);
            fprintf(sf, "  \"stale\": %" PRIu64 ",\n", acc.stale);
            fprintf(sf, "  \"signer_busy\": %" PRIu64 ",\n", acc.signer_busy);
            fprintf(sf, "  \"burst_max_1ms\": %u,\n", acc.burst_max);
            fprintf(sf, "  \"act_n\": %u, \"act_p50_ns\": %u, \"act_p99_ns\": %u, \"act_p999_ns\": %u,\n",
                    acc.n_act, pct_u32(acc.act, acc.n_act, 0.50),
                    pct_u32(acc.act, acc.n_act, 0.99),
                    pct_u32(acc.act, acc.n_act, 0.999));
            fprintf(sf, "  \"sgn_n\": %u, \"sgn_p50_ns\": %u, \"sgn_p99_ns\": %u, \"sgn_p999_ns\": %u,\n",
                    acc.n_sgn, pct_u32(acc.sgn, acc.n_sgn, 0.50),
                    pct_u32(acc.sgn, acc.n_sgn, 0.99),
                    pct_u32(acc.sgn, acc.n_sgn, 0.999));
            fprintf(sf, "  \"winner_n\": %u, \"winner_seen\": %u,\n", wins.n, wins.found);
            fprintf(sf, "  \"pools\": %u, \"routes\": %u, \"paced\": %d\n",
                    u.n, u.routes.n_route, paced);
            fprintf(sf, "}\n");
            fclose(sf);
        }
    }
    if (jpath != NULL && wins.tab != NULL) {
        char wout[1024];
        FILE *wf;
        uint32_t i;
        snprintf(wout, sizeof(wout), "%s.winners.jsonl", jpath);
        wf = fopen(wout, "w");
        if (wf != NULL) {
            for (i = 0; i < wins.cap; i++) {
                uint32_t b;
                if (!wins.tab[i].seen) {
                    continue;
                }
                fprintf(wf, "{\"slot\":%u,\"rx_ns\":%" PRIu64 ",\"sig\":\"",
                        wins.tab[i].slot, wins.tab[i].rx_ns);
                for (b = 0; b < 64; b++) {
                    fprintf(wf, "%02x", wins.tab[i].sig[b]);
                }
                fprintf(wf, "\"}\n");
            }
            fclose(wf);
        }
    }
    if (jrn != NULL) {
        fclose(jrn);
    }
    if (sync_path != NULL) {
        syncrec_close(&sync);
    }
    hot_seen_free(&seen);
    free(wins.tab);
    univ_state_free(&st);
    live_univ_free(&u);
    free(acc.act);
    free(acc.sgn);
    return 0;
}
