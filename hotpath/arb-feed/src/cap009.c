/*
 * CAP-009 — exact hot path on the raw FEEDCAP1 corpus.
 * Does not write the .cap. Locked classify.c / route0 are not mutated.
 *
 *   raw .cap → rx/slot → shred → classify_n → decode → pool_idx
 *     → apply → routes_by_pool → quote → size → opportunity_t
 *     → nonce → template → sign → leader_send(STUB)
 */
#include "classify.h"
#include "classify_n.h"
#include "feed.h"
#include "hot.h"
#include "nonce.h"
#include "proto.h"
#include "route_fam.h"
#include "shred.h"
#include "sign.h"
#include "tsc.h"
#include "tx_n.h"
#include "universe.h"
#include "util.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t
cyc_to_ns(const struct tsc_clock *c, uint64_t cyc)
{
    if (c == NULL || c->hz <= 0.0) {
        return 0;
    }
    return (uint64_t)((double)cyc * 1e9 / c->hz);
}

#define MIN_CAP_BYTES   (1u << 20)
#define LAT_MAX         (1u << 20)
#define CLASS_BINS      256u
#define CLASS_BIN_W     8u

typedef struct {
    uint64_t shred;
    uint64_t bad;
    uint64_t rel[7];
    uint64_t framed;
    uint64_t apply_ok;
    uint64_t eval_ok;
    uint64_t opp_valid;
    uint64_t signed_ok;
    uint64_t send_ok;
    uint64_t class_cyc[7][CLASS_BINS];
    uint64_t class_n[7];
    uint64_t class_sum[7];
    uint32_t *act_ns;
    uint32_t n_act;
    uint32_t *sign_ns;
    uint32_t n_sign;
    uint32_t *univ_ns[5];
    uint32_t n_univ[5];
    uint32_t proto_hit[PROTO_N];
} cap009_t;

static uint8_t g_sink[ROUTE0_TX_LEN];
static uint64_t g_sent;

static int
leader_send_stub(const uint8_t *tx, uint16_t len)
{
    if (tx == NULL || len == 0 || len > ROUTE0_TX_LEN) {
        return -1;
    }
    memcpy(g_sink, tx, len);
    g_sent++;
    return 0;
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

static uint64_t
pct_hist(const uint64_t *h, uint32_t bins, uint64_t n, double p)
{
    uint64_t want;
    uint64_t acc = 0;
    uint32_t i;

    if (n == 0) {
        return 0;
    }
    want = (uint64_t)(p * (double)(n - 1u));
    for (i = 0; i < bins; i++) {
        acc += h[i];
        if (acc > want) {
            return (uint64_t)i * CLASS_BIN_W + CLASS_BIN_W / 2u;
        }
    }
    return (uint64_t)(bins - 1u) * CLASS_BIN_W;
}

static void
note_class(cap009_t *c, uint32_t n_ids, uint64_t cyc)
{
    uint32_t bin = (uint32_t)(cyc / CLASS_BIN_W);
    if (bin >= CLASS_BINS) {
        bin = CLASS_BINS - 1u;
    }
    c->class_cyc[n_ids][bin]++;
    c->class_n[n_ids]++;
    c->class_sum[n_ids] += cyc;
}

static uint32_t
map_pool(const universe_t *u, uint8_t proto, const uint8_t *key)
{
    uint32_t i;
    uint32_t first = UINT32_MAX;
    uint32_t n = 0;
    uint32_t h;

    for (i = 0; i < u->n_pool; i++) {
        if (u->pool[i].protocol == proto) {
            if (first == UINT32_MAX) {
                first = i;
            }
            n++;
        }
    }
    if (n == 0 || first == UINT32_MAX) {
        return UINT32_MAX;
    }
    memcpy(&h, key, 4);
    return first + (h % n);
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int
walk_file(cap009_t *c, universe_t *univ, univ_state_t *st,
          route_fam_t *fam, route0_signer_t *signer, nonce_pool_t *nonces,
          const struct tsc_clock *clk, uint32_t n_ids, const char *path)
{
    FILE *f;
    feedcap_hdr_t hdr;
    feed_slot_t slot;
    long sz;
    uint8_t tx[ROUTE0_TX_LEN];
    uint8_t dummy_bh[32];

    memset(dummy_bh, 0x5a, 32);
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < (long)(FEEDCAP_HDR_LEN + MIN_CAP_BYTES)) {
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
        uint32_t rel = REL_N_NONE;
        uint32_t k;
        int rc = feedcap_read_slot(f, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fclose(f);
            return -1;
        }
        if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
            c->bad++;
            continue;
        }
        c->shred++;
        plen = (uint16_t)(slot.len - (uint16_t)(v.payload - slot.data));

        for (k = 2; k <= 6; k++) {
            uint64_t t0 = rdtscp();
            uint32_t r = classify_n(v.payload, plen, k);
            uint64_t cyc = rdtscp() - t0;
            note_class(c, k, cyc);
            if (k == n_ids) {
                rel = r;
            }
            c->rel[k] += (r != REL_N_NONE);
        }

        if (rel == REL_N_NONE) {
            continue;
        }

        {
            uint8_t proto;
            const uint8_t *pkey;
            uint32_t pidx;
            opportunity_t opp;
            nonce_claim_t claim;
            uint64_t t_act = rdtscp();
            uint8_t family;

            t_act = rdtscp();
            if (msg_first_dex_pool_n(v.payload, plen, n_ids, &proto, &pkey) != 0) {
                continue;
            }
            c->framed++;
            if (proto < PROTO_N) {
                c->proto_hit[proto]++;
            }
            pidx = map_pool(&univ[n_ids - 2u], proto, pkey);
            if (pidx == UINT32_MAX) {
                continue;
            }
            if (hot_apply_pool(&univ[n_ids - 2u], &st[n_ids - 2u],
                               pidx, 10000000ull, 0) == 0) {
                c->apply_ok++;
            }
            if (hot_eval_pool(&univ[n_ids - 2u], &st[n_ids - 2u],
                              pidx, &opp) == 0) {
                c->eval_ok++;
            }
            if (opp.valid) {
                c->opp_valid++;
            } else {
                opp.route_id = 0;
                opp.amount_in = 10000000ull;
                opp.amount_out = 10000001ull;
                opp.gross_profit = 1;
                opp.direction = 0;
                opp.valid = 1;
            }
            if (c->n_act < LAT_MAX) {
                c->act_ns[c->n_act++] =
                    (uint32_t)cyc_to_ns(clk, rdtscp() - t_act);
            }

            family = 0;
            if (opp.route_id < univ[n_ids - 2u].n_route) {
                family = univ[n_ids - 2u].route[opp.route_id].family;
                if (family >= ROUTE_FAM_N) {
                    family = 0;
                }
            }
            if (nonce_claim(nonces, &claim) != 0) {
                continue;
            }
            if (route_fam_patch(fam, family, &opp, 1, claim.hash, 1000, tx) != 0) {
                (void)nonce_reload(nonces, claim.idx, dummy_bh);
                continue;
            }
            if (route0_sign(signer, tx) == 0) {
                c->signed_ok++;
                if (leader_send_stub(tx, ROUTE0_TX_LEN) == 0) {
                    c->send_ok++;
                }
            }
            (void)nonce_reload(nonces, claim.idx, dummy_bh);
            if (c->n_sign < LAT_MAX) {
                c->sign_ns[c->n_sign++] =
                    (uint32_t)cyc_to_ns(clk, rdtscp() - t_act);
            }
            {
                uint32_t ui;
                for (ui = 0; ui < 5; ui++) {
                    uint32_t pi = map_pool(&univ[ui], proto, pkey);
                    opportunity_t o;
                    uint64_t t0;
                    if (pi == UINT32_MAX) {
                        continue;
                    }
                    t0 = rdtscp();
                    (void)hot_apply_pool(&univ[ui], &st[ui], pi, 10000000ull, 0);
                    (void)hot_eval_pool(&univ[ui], &st[ui], pi, &o);
                    if (c->n_univ[ui] < LAT_MAX) {
                        c->univ_ns[ui][c->n_univ[ui]++] =
                            (uint32_t)cyc_to_ns(clk, rdtscp() - t0);
                    }
                }
            }
        }
    }
    fclose(f);
    return 0;
}

static int
walk_dir(cap009_t *c, universe_t *u, univ_state_t *st, route_fam_t *fam,
         route0_signer_t *signer, nonce_pool_t *nonces,
         const struct tsc_clock *clk, uint32_t n_ids, const char *dir)
{
    DIR *d;
    struct dirent *ent;
    char **paths = NULL;
    uint32_t n = 0, cap = 0, i;
    int rc = 0;

    d = opendir(dir);
    if (d == NULL) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        char *p;
        if (strstr(ent->d_name, "shredstream-") != ent->d_name) {
            continue;
        }
        if (len < 4 || strcmp(ent->d_name + len - 4, ".cap") != 0) {
            continue;
        }
        if (n == cap) {
            uint32_t ncap = cap == 0 ? 64 : cap * 2;
            char **np = realloc(paths, ncap * sizeof(*np));
            if (np == NULL) {
                rc = -1;
                break;
            }
            paths = np;
            cap = ncap;
        }
        p = malloc(strlen(dir) + 1 + len + 1);
        if (p == NULL) {
            rc = -1;
            break;
        }
        sprintf(p, "%s/%s", dir, ent->d_name);
        paths[n++] = p;
    }
    closedir(d);
    if (rc == 0) {
        qsort(paths, n, sizeof(*paths), cmp_str);
        for (i = 0; i < n; i++) {
            if (walk_file(c, u, st, fam, signer, nonces, clk, n_ids,
                          paths[i]) != 0) {
                rc = -1;
                break;
            }
        }
    }
    for (i = 0; i < n; i++) {
        free(paths[i]);
    }
    free(paths);
    return rc;
}

static void
report_class(const cap009_t *c)
{
    uint32_t k;
    printf("\nclassifier  (all shreds, hist p50/p99 cycles)\n");
    printf("IDs   n            p50    p99    mean\n");
    for (k = 2; k <= 6; k++) {
        uint64_t mean = c->class_n[k] ? c->class_sum[k] / c->class_n[k] : 0;
        printf("%u     %-12" PRIu64 " %5" PRIu64 "  %5" PRIu64 "  %5" PRIu64 "\n",
               k, c->class_n[k],
               pct_hist(c->class_cyc[k], CLASS_BINS, c->class_n[k], 0.50),
               pct_hist(c->class_cyc[k], CLASS_BINS, c->class_n[k], 0.99),
               mean);
    }
}

int
main(int argc, char **argv)
{
    cap009_t c;
    universe_t u[5];
    univ_state_t st[5];
    route_fam_t fam;
    route0_signer_t signer;
    nonce_pool_t nonces;
    struct tsc_clock clk;
    const char *dir = NULL;
    uint32_t n_ids = 6;
    uint8_t seed[32];
    uint8_t npub[32];
    static const uint32_t MASK[5] = {
        UNIV_MASK_2, UNIV_MASK_3, UNIV_MASK_4, UNIV_MASK_5, UNIV_MASK_6
    };
    static const char *ULAB[5] = {
        "DLMM+Pump", "+CLMM", "+CPMM", "+DAMM", "+Orca"
    };
    uint32_t i;
    int ai;

    memset(&c, 0, sizeof(c));
    memset(u, 0, sizeof(u));
    memset(st, 0, sizeof(st));
    memset(seed, 0x42, 32);
    memset(npub, 0x43, 32);
    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--dir") == 0 && ai + 1 < argc) {
            dir = argv[++ai];
        } else if (strcmp(argv[ai], "--ids") == 0 && ai + 1 < argc) {
            n_ids = (uint32_t)atoi(argv[++ai]);
        } else {
            fprintf(stderr, "usage: %s --dir DIR [--ids 2..6]\n", argv[0]);
            return 1;
        }
    }
    if (dir == NULL || n_ids < 2 || n_ids > 6) {
        fprintf(stderr, "usage: %s --dir DIR [--ids 2..6]\n", argv[0]);
        return 1;
    }
    c.act_ns = calloc(LAT_MAX, sizeof(*c.act_ns));
    c.sign_ns = calloc(LAT_MAX, sizeof(*c.sign_ns));
    if (c.act_ns == NULL || c.sign_ns == NULL) {
        return 1;
    }
    for (i = 0; i < 5; i++) {
        c.univ_ns[i] = calloc(LAT_MAX, sizeof(*c.univ_ns[i]));
        if (c.univ_ns[i] == NULL) {
            return 1;
        }
    }
    if (tsc_calibrate(&clk) != 0) {
        fprintf(stderr, "tsc calibrate failed\n");
        return 1;
    }
    for (i = 0; i < 5; i++) {
        if (universe_init(&u[i], 256, 65536) != 0
            || universe_seed(&u[i], i + 2u) != 0
            || universe_compile(&u[i], MASK[i]) != 0
            || univ_state_init(&st[i], u[i].n_pool) != 0) {
            fprintf(stderr, "universe %u failed\n", i + 2u);
            return 1;
        }
        universe_fill_synthetic(&u[i], &st[i]);
        printf("universe %-10s  pools=%u  routes=%u\n",
               ULAB[i], u[i].n_pool, u[i].n_route);
    }
    if (route_fam_init(&fam) != 0) {
        fprintf(stderr, "route_fam_init failed\n");
        return 1;
    }
    if (route0_signer_init(&signer, seed) != 0) {
        fprintf(stderr, "signer failed\n");
        return 1;
    }
    nonce_pool_init(&nonces);
    for (i = 0; i < NONCE_POOL_N; i++) {
        uint8_t h[32];
        memset(h, (int)i, 32);
        if (nonce_load(&nonces, i, npub, h) != 0) {
            return 1;
        }
    }

    printf("CAP-009  n_ids=%u  tsc_hz=%.0f\n", n_ids, clk.hz);
    if (walk_dir(&c, u, st, &fam, &signer, &nonces, &clk, n_ids, dir) != 0) {
        return 1;
    }

    printf("shred_ok           %" PRIu64 "\n", c.shred);
    printf("framed             %" PRIu64 "\n", c.framed);
    printf("apply_ok           %" PRIu64 "\n", c.apply_ok);
    printf("eval_ok            %" PRIu64 "\n", c.eval_ok);
    printf("opp_valid          %" PRIu64 "\n", c.opp_valid);
    printf("signed             %" PRIu64 "\n", c.signed_ok);
    printf("leader_stub        %" PRIu64 "\n", c.send_ok);
    for (i = 1; i < PROTO_N; i++) {
        static const char *nm[] = {
            "none", "dlmm", "pump", "clmm", "cpmm", "damm", "orca"
        };
        printf("framed_%-8s     %" PRIu32 "\n", nm[i], c.proto_hit[i]);
    }
    printf("rel_ids2..6       ");
    for (i = 2; i <= 6; i++) {
        printf(" %" PRIu64, c.rel[i]);
    }
    printf("\n");
    report_class(&c);
    printf("\nactionable -> opp     n=%u  p50=%u ns  p99=%u ns  p999=%u ns\n",
           c.n_act, pct_u32(c.act_ns, c.n_act, 0.50),
           pct_u32(c.act_ns, c.n_act, 0.99),
           pct_u32(c.act_ns, c.n_act, 0.999));
    printf("actionable -> signed  n=%u  p50=%u ns  p99=%u ns  p999=%u ns\n",
           c.n_sign, pct_u32(c.sign_ns, c.n_sign, 0.50),
           pct_u32(c.sign_ns, c.n_sign, 0.99),
           pct_u32(c.sign_ns, c.n_sign, 0.999));
    printf("\nUniverse            Pools  Routes      p50     p99    p999\n");
    for (i = 0; i < 5; i++) {
        uint32_t n = c.n_univ[i];
        printf("%-18s %5u  %6u  %6u ns %6u ns %6u ns\n",
               ULAB[i], u[i].n_pool, u[i].n_route,
               pct_u32(c.univ_ns[i], n, 0.50),
               pct_u32(c.univ_ns[i], n, 0.99),
               pct_u32(c.univ_ns[i], n, 0.999));
    }
    if (c.n_sign > 0 && pct_u32(c.sign_ns, c.n_sign, 0.50) > 40000u) {
        printf("STOP  signed p50 left 20us-class. Diagnose before adding venues.\n");
    }
    for (i = 0; i < 5; i++) {
        universe_free(&u[i]);
        univ_state_free(&st[i]);
        free(c.univ_ns[i]);
    }
    free(c.act_ns);
    free(c.sign_ns);
    return 0;
}
