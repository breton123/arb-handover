/*
 * PAPER-ORBIT — follow OrbitFlare FEEDCAP1 while feed_live keeps writing it.
 * Does not bind 20001. Does not write .cap. Does not send.
 * Fresh liveuniv → STATE_INIT → unique N → state_observe (S' only).
 * STATE-006: confirm/reject/refresh only from the off-path recon file.
 */
#include "classify_n.h"
#include "cycle.h"
#include "feed.h"
#include "frame.h"
#include "alt_cache.h"
#include "hot.h"
#include "hops_hurdle.h"
#include "live.h"
#include "paper.h"
#include "universe_gen.h"
#include "trigger.h"
#include "watch.h"
#include "pool.h"
#include "route0_v0.h"
#include "shred.h"
#include "proto.h"
#include "state003.h"
#include "authpub.h"
#include "swapix.h"
#include "tx_overlay.h"
#include "syncrec.h"
#include "tsc.h"
#include "tx_template.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LAT_MAX     (1u << 20)
#define FLOW_CAP    (1u << 20)
#define FEC_CAP     4096u
#define RATE_CAP    2048u
#define PEND_CAP    4096u
#define RECON_HDR   8u
#define RECON_FIXED 80u
#define RECON_REJECT 1u
#define RECON_LAND   2u
#define RECON_AUTH   3u
#define RECON_INVALIDATE 4u
#define RECON_PLANE  5u
#define RECON_MAGIC  0x36305453u
#define F_SLOTS     4u
#define F_SHREDS    2048u
#define F_ARENA     (2u * 1024u * 1024u)
#define HOLD_MAX    32u
#define FRESH_MAX_SLOTS ((uint64_t)32)
/* Per-sequence hurdle from hops_hurdle.h. No universal 525k. */

typedef struct {
    uint8_t  used;
    uint8_t  sig[64];
    uint64_t flow_mono_ns;
} flow_ent_t;

typedef struct {
    uint64_t shred;
    uint64_t bad;
    uint64_t data;
    uint64_t coding;
    uint64_t relevant;
    uint64_t decoded;
    uint64_t decode_fail;
    uint64_t dup;
    uint64_t known;
    uint64_t unknown;
    uint64_t state_ok;
    uint64_t missing_state;
    uint64_t decide;
    uint64_t opp;
    uint64_t opp_synced;
    uint64_t signed_ready;
    uint64_t stale;
    uint64_t slot_gap;
    uint64_t fec_incomplete;
    uint64_t both;
    uint64_t frame_invalid;
    uint64_t frame_incomplete;
    uint64_t frame_framed;
    uint64_t frame_expired;
    uint64_t fresh_drop;
    uint64_t searchable;
    uint64_t fun_ev0;
    uint64_t fun_fam5;
    uint64_t fun_hop3;
    uint64_t fun_state;
    uint64_t fun_age32;
    uint64_t fun_mut_auth;
    uint64_t fun_would_new;
    uint64_t fun_updating;
    uint64_t trig_generic;
    uint64_t trig_exact;
    uint64_t trig_unknown;
    uint64_t trig_alt_miss;
    uint64_t auth_pub_ok;
    uint64_t auth_pub_miss;
    uint64_t ix_exact;
    uint64_t tx_exact;
    uint64_t lead_n;
    int64_t  lead_sum;
    int64_t  lead_min;
    int64_t  lead_max;
    uint32_t *act;
    uint32_t n_act;
    uint32_t *sgn;
    uint32_t n_sgn;
    uint32_t *fdelay;
    uint32_t n_fdelay;
    uint32_t rate[RATE_CAP];
    uint32_t n_rate;
    uint32_t rate_cur;
    uint64_t rate_win;
    uint64_t last_slot;
    uint64_t first_slot;
} acc_t;

typedef struct {
    uint8_t        used;
    uint8_t        sig[64];
    uint32_t       pool_idx;
    hot_trigger_t  trig;
} pend_ent_t;

typedef struct {
    pend_ent_t e[PEND_CAP];
    FILE      *pend;
    FILE      *recon;
    long       recon_off;
    const char *recon_path;
} pend_q_t;

typedef struct {
    uint64_t slot;
    uint32_t fec;
    uint32_t data;
    uint32_t code;
    uint8_t  used;
} fec_row_t;

typedef struct {
    uint8_t  nsig;
    uint16_t nkeys;
    uint16_t ninstr;
    uint64_t t0_ns;
    uint64_t framed_ns;
} frame_info_t;

typedef struct {
    uint32_t index;
    uint16_t plen;
    uint32_t off;
    uint64_t rx_ns;
} fshred_t;

typedef struct {
    uint64_t slot;
    uint8_t  used;
    uint32_t dn;
    uint32_t arena_used;
    uint8_t *arena;
    fshred_t d[F_SHREDS];
} fslot_t;

typedef struct {
    uint8_t       used;
    uint8_t       sig[64];
    uint64_t      slot;
    uint32_t      index;
    uint64_t      t0_ns;
    swapix_t      nix;
    hot_trigger_t trig;
} hold_ent_t;

typedef struct {
    live_univ_t *u;
    state_mgr_t *smgr;
    syncrec_t *sync;
    pend_q_t *pq;
    acc_t *acc;
    hot_seen_t *seen;
    route0_v0_template_t *v0;
    route0_signer_t *signer;
    struct tsc_clock *clk;
    FILE *auditf;
    univ_state_t *st;
    alt_cache_t *alts;
    watch_idx_t *watch;
    trigger_metrics_t *trigm;
    authpub_meta_t last_auth;
} race_ctx_t;

static fslot_t g_fslot[F_SLOTS];
static hold_ent_t g_hold[HOLD_MAX];
static uint8_t g_fbuf[F_ARENA];
#ifndef FUNDED
#define FUNDED 0
#endif

static uint64_t *g_mut_slot;
static uint8_t  g_s007_ready;
static uint8_t  *g_s007_updating;
static uint64_t *g_s007_gen;
static uint64_t *g_s007_rx_ns;
static uint32_t  g_pool_cap;

#define POOL_OK(i) ((i) < g_pool_cap)

static int
runtime_pools(uint32_t n)
{
    if (n == 0) {
        return -1;
    }
    g_pool_cap = n;
    g_mut_slot = calloc(n, sizeof(*g_mut_slot));
    g_s007_updating = calloc(n, sizeof(*g_s007_updating));
    g_s007_gen = calloc(n, sizeof(*g_s007_gen));
    g_s007_rx_ns = calloc(n, sizeof(*g_s007_rx_ns));
    return (g_mut_slot != NULL && g_s007_updating != NULL
            && g_s007_gen != NULL && g_s007_rx_ns != NULL) ? 0 : -1;
}

static void
journal_discover(const uint8_t pool[32], uint8_t proto)
{
    static FILE *df;
    char pk[65];
    uint32_t i;

    if (pool == NULL) {
        return;
    }
    if (df == NULL) {
        df = fopen("/home/louis/captures/paper_orbit/discover.jsonl", "a");
    }
    if (df == NULL) {
        return;
    }
    for (i = 0; i < 32; i++) {
        static const char *hx = "0123456789abcdef";
        pk[i * 2] = hx[pool[i] >> 4];
        pk[i * 2 + 1] = hx[pool[i] & 0xf];
    }
    pk[64] = 0;
    fprintf(df, "{\"pool_hex\":\"%s\",\"proto\":%u}\n", pk, (unsigned)proto);
    fflush(df);
}

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

static uint32_t
sig_hash(const uint8_t *s)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0; i < 64; i++) {
        h ^= s[i];
        h *= 16777619u;
    }
    return h;
}

static int
newest_cap(const char *dir, char *out, size_t cap)
{
    DIR *dp;
    struct dirent *de;
    time_t best = 0;
    char name[256];
    int found = 0;

    dp = opendir(dir);
    if (dp == NULL) {
        return -1;
    }
    name[0] = 0;
    while ((de = readdir(dp)) != NULL) {
        size_t L = strlen(de->d_name);
        char path[1024];
        struct stat st;
        if (L < 5 || strcmp(de->d_name + L - 4, ".cap") != 0) {
            continue;
        }
        if (strncmp(de->d_name, "orbitflare-", 11) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (st.st_mtime >= best) {
            best = st.st_mtime;
            snprintf(name, sizeof(name), "%s", de->d_name);
            found = 1;
        }
    }
    closedir(dp);
    if (!found) {
        return -1;
    }
    snprintf(out, cap, "%s/%s", dir, name);
    return 0;
}

static int
follow_slot(FILE *f, feed_slot_t *s)
{
    long pos;
    uint8_t hdr[FEEDCAP_REC_HDR];
    shred_view_t v;
    size_t n;

    pos = ftell(f);
    if (pos < 0) {
        return -1;
    }
    n = fread(hdr, 1, FEEDCAP_REC_HDR, f);
    if (n != FEEDCAP_REC_HDR) {
        clearerr(f);
        if (fseek(f, pos, SEEK_SET) != 0) {
            return -1;
        }
        return 1;
    }
    memcpy(&s->rx_ns, hdr, 8);
    memcpy(&s->rx_tsc, hdr + 8, 8);
    memcpy(&s->len, hdr + 16, 4);
    memcpy(&s->seq, hdr + 20, 4);
    if (s->len < SHRED_COMMON_HDR_SZ || s->len > SHRED_MAX_SZ) {
        if (fseek(f, pos + 1, SEEK_SET) != 0) {
            return -1;
        }
        return 2;
    }
    n = fread(s->data, 1, s->len, f);
    if (n != s->len) {
        clearerr(f);
        if (fseek(f, pos, SEEK_SET) != 0) {
            return -1;
        }
        return 1;
    }
    if (shred_parse(s->data, (uint16_t)s->len, &v) != 0) {
        if (fseek(f, pos + 1, SEEK_SET) != 0) {
            return -1;
        }
        return 2;
    }
    return 0;
}

static void
flow_load(flow_ent_t *tab, uint32_t cap, const char *path, uint64_t *n)
{
    FILE *f;
    uint8_t rec[88];
    uint32_t mask = cap - 1u;

    if (path == NULL || path[0] == 0) {
        return;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    while (fread(rec, 1, 88, f) == 88) {
        uint32_t h = sig_hash(rec) & mask;
        uint32_t k = h;
        for (;;) {
            if (!tab[k].used) {
                tab[k].used = 1;
                memcpy(tab[k].sig, rec, 64);
                memcpy(&tab[k].flow_mono_ns, rec + 72, 8);
                (*n)++;
                break;
            }
            if (memcmp(tab[k].sig, rec, 64) == 0) {
                break;
            }
            k = (k + 1u) & mask;
            if (k == h) {
                break;
            }
        }
    }
    fclose(f);
}

static int
flow_find(const flow_ent_t *tab, uint32_t cap, const uint8_t sig[64], uint64_t *mono)
{
    uint32_t mask = cap - 1u;
    uint32_t h = sig_hash(sig) & mask;
    uint32_t k = h;
    for (;;) {
        if (!tab[k].used) {
            return 0;
        }
        if (memcmp(tab[k].sig, sig, 64) == 0) {
            *mono = tab[k].flow_mono_ns;
            return 1;
        }
        k = (k + 1u) & mask;
        if (k == h) {
            return 0;
        }
    }
}

static void
fec_note(fec_row_t *t, uint32_t cap, const shred_view_t *v, int is_data)
{
    uint32_t i = (uint32_t)((v->slot ^ ((uint64_t)v->fec_set * 16777619ull)) % cap);
    uint32_t k;
    for (k = 0; k < 16; k++) {
        fec_row_t *r = &t[(i + k) % cap];
        if (!r->used || (r->slot == v->slot && r->fec == v->fec_set)) {
            if (!r->used) {
                r->used = 1;
                r->slot = v->slot;
                r->fec = v->fec_set;
            }
            if (is_data) {
                r->data++;
            } else {
                r->code++;
            }
            return;
        }
    }
}

static void
fec_tally(const fec_row_t *t, uint32_t cap, acc_t *a)
{
    uint32_t i;
    a->fec_incomplete = 0;
    for (i = 0; i < cap; i++) {
        if (t[i].used && (t[i].data == 0 || t[i].code == 0)) {
            a->fec_incomplete++;
        }
    }
}

static int
list_idx(const char *dir, char paths[][1024], int max)
{
    DIR *dp;
    struct dirent *de;
    int n = 0;

    if (dir == NULL) {
        return 0;
    }
    dp = opendir(dir);
    if (dp == NULL) {
        return 0;
    }
    while ((de = readdir(dp)) != NULL && n < max) {
        size_t L = strlen(de->d_name);
        if (L < 5 || strcmp(de->d_name + L - 4, ".idx") != 0) {
            continue;
        }
        snprintf(paths[n], 1024, "%s/%s", dir, de->d_name);
        n++;
    }
    closedir(dp);
    return n;
}

static void
hex_encode(const uint8_t *b, size_t n, char *out)
{
    static const char *H = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2u] = H[b[i] >> 4];
        out[i * 2u + 1u] = H[b[i] & 15u];
    }
    out[n * 2u] = 0;
}

static const char *
frame_name(int klass)
{
    if (klass == FRAME_FRAMED) {
        return "framed";
    }
    if (klass == FRAME_INCOMPLETE) {
        return "incomplete";
    }
    if (klass == FRAME_INVALID) {
        return "invalid";
    }
    return "expired";
}

static void
audit_frame(FILE *af, const uint8_t sig[64], const shred_view_t *sv,
            int klass, uint64_t t0_ns, uint64_t now, const frame_hit_t *hit)
{
    char hx[129];
    uint64_t delay;

    if (af == NULL || sig == NULL) {
        return;
    }
    hex_encode(sig, 64, hx);
    delay = (now >= t0_ns) ? (now - t0_ns) : 0;
    fprintf(af,
            "{\"kind\":\"frame\",\"class\":\"%s\",\"sig_hex\":\"%s\""
            ",\"t0_ns\":%" PRIu64 ",\"now_ns\":%" PRIu64
            ",\"delay_ns\":%" PRIu64
            ",\"shred\":{\"slot\":%" PRIu64 ",\"index\":%u}"
            ",\"nsig\":%u,\"nkeys\":%u,\"ninstr\":%u}\n",
            frame_name(klass), hx, t0_ns, now, delay,
            sv != NULL ? sv->slot : (uint64_t)0,
            sv != NULL ? sv->index : 0u,
            hit != NULL ? (unsigned)hit->nsig : 0u,
            hit != NULL ? (unsigned)hit->nkeys : 0u,
            hit != NULL ? (unsigned)hit->ninstr : 0u);
    fflush(af);
}

static fshred_t *
fshred_find(fslot_t *s, uint32_t idx)
{
    uint32_t i;
    if (s == NULL) {
        return NULL;
    }
    for (i = 0; i < s->dn; i++) {
        if (s->d[i].index == idx) {
            return &s->d[i];
        }
    }
    return NULL;
}

static void
hold_expire_slot(uint64_t slot, FILE *af, acc_t *acc)
{
    uint32_t i;
    for (i = 0; i < HOLD_MAX; i++) {
        if (g_hold[i].used && g_hold[i].slot == slot) {
            audit_frame(af, g_hold[i].sig, NULL, 3, g_hold[i].t0_ns,
                        now_ns(), NULL);
            g_hold[i].used = 0;
            if (acc != NULL) {
                acc->frame_expired++;
            }
        }
    }
}

static fslot_t *
fslot_get(uint64_t slot, FILE *af, acc_t *acc)
{
    uint32_t i, victim = 0;
    uint64_t oldest = UINT64_MAX;

    for (i = 0; i < F_SLOTS; i++) {
        if (g_fslot[i].used && g_fslot[i].slot == slot) {
            return &g_fslot[i];
        }
    }
    for (i = 0; i < F_SLOTS; i++) {
        if (!g_fslot[i].used) {
            victim = i;
            oldest = 0;
            break;
        }
        if (g_fslot[i].slot < oldest) {
            oldest = g_fslot[i].slot;
            victim = i;
        }
    }
    if (g_fslot[victim].used) {
        hold_expire_slot(g_fslot[victim].slot, af, acc);
    }
    g_fslot[victim].slot = slot;
    g_fslot[victim].used = 1;
    g_fslot[victim].dn = 0;
    g_fslot[victim].arena_used = 0;
    return &g_fslot[victim];
}

static int
fslot_store(fslot_t *s, uint32_t idx, const uint8_t *p, uint16_t plen,
            uint64_t rx)
{
    if (s == NULL || p == NULL || plen == 0) {
        return -1;
    }
    if (fshred_find(s, idx) != NULL) {
        return 0;
    }
    if (s->dn >= F_SHREDS || s->arena == NULL
        || s->arena_used + (uint32_t)plen > F_ARENA) {
        return -1;
    }
    memcpy(s->arena + s->arena_used, p, plen);
    s->d[s->dn].index = idx;
    s->d[s->dn].plen = plen;
    s->d[s->dn].off = s->arena_used;
    s->d[s->dn].rx_ns = rx;
    s->arena_used += plen;
    s->dn++;
    return 0;
}

static uint32_t
frame_concat(fslot_t *s, uint32_t around)
{
    uint32_t lo = around, hi = around, n = 0, idx;
    fshred_t *r;

    if (s == NULL || fshred_find(s, around) == NULL) {
        return 0;
    }
    while (lo > 0u && fshred_find(s, lo - 1u) != NULL) {
        lo--;
    }
    while (fshred_find(s, hi + 1u) != NULL) {
        hi++;
    }
    for (idx = lo; idx <= hi; idx++) {
        r = fshred_find(s, idx);
        if (r == NULL || n + r->plen > F_ARENA) {
            return 0;
        }
        memcpy(g_fbuf + n, s->arena + r->off, r->plen);
        n += r->plen;
    }
    return n;
}

static hold_ent_t *
hold_find(const uint8_t sig[64])
{
    uint32_t i;
    for (i = 0; i < HOLD_MAX; i++) {
        if (g_hold[i].used && memcmp(g_hold[i].sig, sig, 64) == 0) {
            return &g_hold[i];
        }
    }
    return NULL;
}

static hold_ent_t *
hold_put(const uint8_t sig[64], uint64_t slot, uint32_t index,
         uint64_t t0_ns, const swapix_t *nix, const hot_trigger_t *trig)
{
    hold_ent_t *e = hold_find(sig);
    uint32_t i;
    if (e == NULL) {
        for (i = 0; i < HOLD_MAX; i++) {
            if (!g_hold[i].used) {
                e = &g_hold[i];
                break;
            }
        }
        if (e == NULL) {
            e = &g_hold[0];
        }
    }
    e->used = 1;
    memcpy(e->sig, sig, 64);
    e->slot = slot;
    e->index = index;
    e->t0_ns = t0_ns;
    e->nix = *nix;
    e->trig = *trig;
    return e;
}

static pend_ent_t *
pend_find(pend_q_t *q, const uint8_t sig[64])
{
    uint32_t i;
    for (i = 0; i < PEND_CAP; i++) {
        if (q->e[i].used && memcmp(q->e[i].sig, sig, 64) == 0) {
            return &q->e[i];
        }
    }
    return NULL;
}

static void
pend_put(pend_q_t *q, const uint8_t sig[64], uint32_t pool_idx,
         const hot_trigger_t *trig)
{
    pend_ent_t *e = pend_find(q, sig);
    uint32_t i;
    if (e == NULL) {
        for (i = 0; i < PEND_CAP; i++) {
            if (!q->e[i].used) {
                e = &q->e[i];
                break;
            }
        }
        if (e == NULL) {
            e = &q->e[0];
        }
    }
    e->used = 1;
    memcpy(e->sig, sig, 64);
    e->pool_idx = pool_idx;
    e->trig = *trig;
}

static void
pend_drop(pend_ent_t *e)
{
    if (e != NULL) {
        e->used = 0;
    }
}

static void
pend_log(pend_q_t *q, const uint8_t sig[64], uint32_t pool_idx, uint8_t proto)
{
    char hx[129];
    if (q->pend == NULL) {
        return;
    }
    hex_encode(sig, 64, hx);
    fprintf(q->pend,
            "{\"sig_hex\":\"%s\",\"pool_idx\":%u,\"proto\":%u}\n",
            hx, pool_idx, (unsigned)proto);
    fflush(q->pend);
}

static int
read_dlmm_mem(const uint8_t *p, size_t n, dlmm_state_t *s)
{
    const uint8_t *end = p + n;
    uint16_t i, nbin;
    memset(s, 0, sizeof(*s));
    if (p + 73 > end) {
        return -1;
    }
    memcpy(&s->active_id, p, 4); p += 4;
    memcpy(&s->bin_step, p, 2); p += 2;
    s->status = *p++;
    memcpy(&s->parameters.base_factor, p, 2); p += 2;
    memcpy(&s->parameters.filter_period, p, 2); p += 2;
    memcpy(&s->parameters.decay_period, p, 2); p += 2;
    memcpy(&s->parameters.reduction_factor, p, 2); p += 2;
    memcpy(&s->parameters.variable_fee_control, p, 4); p += 4;
    memcpy(&s->parameters.max_volatility_accumulator, p, 4); p += 4;
    memcpy(&s->parameters.protocol_share, p, 2); p += 2;
    s->parameters.base_fee_power_factor = *p++;
    s->parameters.collect_fee_mode = *p++;
    memcpy(&s->v_parameters.volatility_accumulator, p, 4); p += 4;
    memcpy(&s->v_parameters.volatility_reference, p, 4); p += 4;
    memcpy(&s->v_parameters.index_reference, p, 4); p += 4;
    memcpy(&s->v_parameters.last_update_timestamp, p, 8); p += 8;
    memcpy(&s->reserve_x, p, 8); p += 8;
    memcpy(&s->reserve_y, p, 8); p += 8;
    memcpy(&s->now_ts, p, 8); p += 8;
    memcpy(&nbin, p, 2); p += 2;
    if (nbin > LIVE_BIN_MAX) {
        return -1;
    }
    s->nbin = nbin;
    if (p + (size_t)nbin * 20u > end) {
        return -1;
    }
    for (i = 0; i < nbin; i++) {
        memcpy(&s->bins[i].id, p, 4); p += 4;
        memcpy(&s->bins[i].amount_x, p, 8); p += 8;
        memcpy(&s->bins[i].amount_y, p, 8); p += 8;
    }
    return 0;
}

static int
read_pump_mem(const uint8_t *p, size_t n, pump_state_t *s)
{
    if (n < 50) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    memcpy(&s->reserve_base, p, 8);
    memcpy(&s->reserve_quote, p + 8, 8);
    memcpy(&s->virtual_quote, p + 16, 8);
    memcpy(&s->lp_fee_bps, p + 24, 8);
    memcpy(&s->protocol_fee_bps, p + 32, 8);
    memcpy(&s->creator_fee_bps, p + 40, 8);
    s->disabled = p[48];
    s->status = p[49];
    return 0;
}

static int
install_auth(race_ctx_t *cx, uint32_t idx, uint64_t trig_slot, const uint8_t *sig)
{
    authpub_meta_t meta;
    uint8_t blob[AUTH_BLOB];
    uint16_t n = 0;

    if (cx == NULL || cx->u == NULL || idx >= cx->u->n) {
        return -1;
    }
    memset(&meta, 0, sizeof(meta));
    if (authpub_pred(idx, trig_slot, sig, &meta, blob, &n) != 0 || !meta.ok) {
        return -1;
    }
    if (cx->u->meta[idx].protocol == PROTO_DLMM) {
        dlmm_state_t auth;
        if (read_dlmm_mem(blob, n, &auth) != 0
            || state_refresh(cx->smgr, cx->u, idx, &auth, meta.slot) != 0) {
            return -1;
        }
    } else if (cx->u->meta[idx].protocol == PROTO_PUMP) {
        pump_state_t auth;
        if (read_pump_mem(blob, n, &auth) != 0
            || state_refresh_pump(cx->smgr, cx->u, idx, &auth, meta.slot) != 0) {
            return -1;
        }
    } else {
        return -1;
    }
    if (POOL_OK(idx)) {
        g_s007_updating[idx] = 0;
        g_s007_gen[idx] = meta.generation;
    }
    cx->last_auth = meta;
    return 0;
}

static int
recon_open(pend_q_t *q, const char *path)
{
    FILE *f;
    uint32_t magic = 0;
    if (path == NULL) {
        return 0;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fread(&magic, 4, 1, f) != 1 || magic != RECON_MAGIC) {
        fclose(f);
        return 0;
    }
    if (fseek(f, (long)RECON_HDR, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    q->recon = f;
    q->recon_off = (long)RECON_HDR;
    return 0;
}

static void
recon_drain(pend_q_t *q, live_univ_t *u, state_mgr_t *m)
{
    uint8_t fixed[RECON_FIXED];
    uint8_t body[4096];
    uint32_t ln;
    uint8_t kind, proto;
    uint32_t pool_idx;
    uint64_t slot;
    pend_ent_t *pe;
    long pos;

    if (q->recon == NULL) {
        (void)recon_open(q, q->recon_path);
        if (q->recon == NULL) {
            return;
        }
    }
    clearerr(q->recon);
    if (fseek(q->recon, 0, SEEK_END) != 0) {
        return;
    }
    pos = ftell(q->recon);
    if (pos < q->recon_off) {
        q->recon_off = (long)RECON_HDR;
    }
    if (fseek(q->recon, q->recon_off, SEEK_SET) != 0) {
        return;
    }
    while (fread(&ln, 4, 1, q->recon) == 1) {
        if (ln < RECON_FIXED || ln > RECON_FIXED + 4000u) {
            break;
        }
        if (fread(fixed, 1, RECON_FIXED, q->recon) != RECON_FIXED) {
            break;
        }
        kind = fixed[0];
        proto = fixed[1];
        memcpy(&pool_idx, fixed + 4, 4);
        memcpy(&slot, fixed + 72, 8);
        pe = pend_find(q, fixed + 8);
        if (kind == RECON_REJECT) {
            if (pool_idx < u->n) {
                (void)state_reject(m, u, pool_idx);
            }
            pend_drop(pe);
        } else if (kind == RECON_LAND && pe != NULL) {
            (void)state_confirm(m, u, pe->pool_idx, &pe->trig);
        } else if (kind == RECON_AUTH && ln > RECON_FIXED
                   && fread(body, 1, ln - RECON_FIXED, q->recon)
                      == ln - RECON_FIXED) {
            /* STATE-009: canonical S is AUTH-PUBLISH, not recon AUTH. */
            (void)proto;
            (void)slot;
            pend_drop(pe);
        } else if (kind == RECON_INVALIDATE) {
            if (ln > RECON_FIXED
                && fread(body, 1, ln - RECON_FIXED, q->recon)
                   != ln - RECON_FIXED) {
                break;
            }
            if (pool_idx < u->n) {
                state_mark_stale(m, pool_idx);
            }
            if (POOL_OK(pool_idx)) {
                uint64_t rx = 0;
                memcpy(&rx, fixed + 8, 8);
                g_s007_updating[pool_idx] = 1;
                g_s007_rx_ns[pool_idx] = rx;
            }
        } else if (kind == RECON_PLANE) {
            if (ln > RECON_FIXED
                && fread(body, 1, ln - RECON_FIXED, q->recon)
                   != ln - RECON_FIXED) {
                break;
            }
            g_s007_ready = (slot != 0ull) ? 1u : 0u;
        } else if (kind == RECON_AUTH) {
            break;
        } else if (ln > RECON_FIXED) {
            if (fseek(q->recon, (long)(ln - RECON_FIXED), SEEK_CUR) != 0) {
                break;
            }
        }
        q->recon_off = ftell(q->recon);
    }
    clearerr(q->recon);
}

static int
mints_pair(const uint8_t *ax, const uint8_t *ay, const uint8_t *bx, const uint8_t *by)
{
    return (memcmp(ax, bx, 32) == 0 && memcmp(ay, by, 32) == 0)
        || (memcmp(ax, by, 32) == 0 && memcmp(ay, bx, 32) == 0);
}

static int
find_route0_partner(const live_univ_t *u, uint32_t hit, uint8_t want, uint32_t *out)
{
    uint32_t i;
    for (i = 0; i < u->n; i++) {
        if (i == hit || u->meta[i].protocol != want) {
            continue;
        }
        if (mints_pair(u->meta[hit].mint_x, u->meta[hit].mint_y,
                       u->meta[i].mint_x, u->meta[i].mint_y)) {
            *out = i;
            return 0;
        }
    }
    return -1;
}

static uint64_t
cycle_mid(const dlmm_state_t *ds, const pump_state_t *ps,
          uint64_t ain, uint8_t dir)
{
    dlmm_quote_t dq;
    pump_quote_t pq;
    if (dir == CYCLE_DLMM_THEN_PUMP) {
        if (dlmm_quote_exact_in(ds, ain, 0, &dq) != 0 || !dq.valid) {
            return 0;
        }
        return dq.amount_out;
    }
    if (pump_quote_exact_in(ps, ain, PUMP_DIR_QUOTE_TO_BASE, &pq) != 0
        || !pq.valid) {
        return 0;
    }
    return pq.amount_out;
}

static void
audit_send_quote(FILE *af, const swapix_t *nix, const shred_view_t *sv,
                 const dlmm_state_t *ds, const pump_state_t *ps, uint8_t dir)
{
    cycle_quote_t qcap;
    cycle_quote_t qmin;
    int64_t fcap = 0, gcap = 0, fmin = 0, gmin = 0;
    unsigned vcap = 0, vmin = 0;

    if (ds != NULL && ps != NULL && dir <= 1u) {
        if (cycle_quote(ds, ps, 50000000ull, dir, &qcap) == 0 && qcap.valid) {
            vcap = 1;
            fcap = (int64_t)qcap.amount_out;
            gcap = qcap.gross_profit;
        }
        if (cycle_quote(ds, ps, 10000000ull, dir, &qmin) == 0 && qmin.valid) {
            vmin = 1;
            fmin = (int64_t)qmin.amount_out;
            gmin = qmin.gross_profit;
        }
    }
    fprintf(af,
            ",\"n_ix\":{\"min_out\":%" PRIu64 ",\"variant\":%u}"
            ",\"shred\":{\"slot\":%" PRIu64 ",\"index\":%u,\"fec\":%u,\"type\":%u}"
            ",\"send_quote\":{\"cap\":50000000,\"cap_ok\":%u,\"cap_final\":%" PRId64
            ",\"cap_gross\":%" PRId64 ",\"tiny\":10000000,\"tiny_ok\":%u,"
            "\"tiny_final\":%" PRId64 ",\"tiny_gross\":%" PRId64 "}",
            nix->min_out, (unsigned)nix->variant,
            sv != NULL ? sv->slot : (uint64_t)0,
            sv != NULL ? sv->index : 0u,
            sv != NULL ? sv->fec_set : 0u,
            sv != NULL ? (unsigned)sv->type : 0u,
            vcap, fcap, gcap, vmin, fmin, gmin);
}

static void
audit_opp_synced(FILE *af, const live_univ_t *u, const state_mgr_t *m,
                 const swapix_t *nix, uint32_t pool_idx,
                 const hot_trigger_t *trig, const opportunity_t *opp,
                 const shred_view_t *sv, const frame_info_t *fi,
                 uint64_t actionable_ns, uint32_t decide_ns, uint32_t signed_ns,
                 uint64_t prior_mut, int race_ready, const authpub_meta_t *am,
                 const tx_overlay_t *ov)
{
    char sig[129], pk[65], asig[129];
    uint32_t partner = UINT32_MAX;
    uint8_t want;
    dlmm_state_t s_dlmm;
    const pump_state_t *ps = NULL;
    const dlmm_state_t *ds = NULL;
    dlmm_state_t s_prime_d;
    pump_state_t s_prime_p;
    int32_t active = 0;
    uint64_t rx = 0, ry = 0;
    uint32_t vacc = 0, vref = 0;
    int32_t iref = 0;
    int64_t last = 0;
    uint64_t fee = 0, pfee = 0;
    int32_t act_after = 0;
    uint16_t crossed = 0;
    uint64_t mid = 0;
    int first_bin = 1;
    uint32_t i;

    memset(&s_prime_d, 0, sizeof(s_prime_d));
    memset(&s_prime_p, 0, sizeof(s_prime_p));
    if (af == NULL || u == NULL || m == NULL || nix == NULL || opp == NULL) {
        return;
    }
    hex_encode(nix->sig, 64, sig);
    hex_encode(u->meta[pool_idx].pubkey, 32, pk);
    if (am != NULL) {
        hex_encode(am->sig, 64, asig);
    } else {
        asig[0] = 0;
    }
    want = (u->meta[pool_idx].protocol == PROTO_DLMM) ? PROTO_PUMP : PROTO_DLMM;
    (void)find_route0_partner(u, pool_idx, want, &partner);

    if (u->meta[pool_idx].protocol == PROTO_DLMM && trig != NULL) {
        dlmm_pool_hot_t spec;
        dlmm_apply_result_t res;
        const dlmm_pool_hot_t *h = dlmm_cache_get(&u->dlmm, pool_idx);
        int used_ov = (ov != NULL && ov->tx_exact && ov->have_dlmm);
        if (used_ov) {
            uint32_t bi;
            s_prime_d = ov->s_dlmm;
            ds = &s_prime_d;
            active = s_prime_d.active_id;
            rx = s_prime_d.reserve_x;
            ry = s_prime_d.reserve_y;
            vacc = s_prime_d.v_parameters.volatility_accumulator;
            vref = s_prime_d.v_parameters.volatility_reference;
            iref = s_prime_d.v_parameters.index_reference;
            last = s_prime_d.v_parameters.last_update_timestamp;
            fee = ov->last_fee;
            pfee = ov->last_pfee;
            act_after = ov->last_active_after;
            if (partner != UINT32_MAX && partner < u->n && u->pump_live[partner]) {
                ps = &u->pump[partner];
            }
            fprintf(af,
                    "{\"kind\":\"opp_synced\",\"sig_hex\":\"%s\",\"pool\":\"%s\""
                    ",\"state_version_before\":%" PRIu64
                    ",\"auth_slot\":%" PRIu64 ",\"auth_ns\":%" PRIu64
                    ",\"n\":{\"pool_idx\":%u,\"proto\":%u,\"direction\":%u,"
                    "\"amount_in\":%" PRIu64 "}"
                    ",\"ix_exact\":%u,\"tx_exact\":%u,\"txo_klass\":%u"
                    ",\"txo_n_leg\":%u,\"txo_n_unknown\":%u"
                    ",\"s_prime\":{\"active_id\":%d,\"active_after\":%d"
                    ",\"reserve_x\":%" PRIu64 ",\"reserve_y\":%" PRIu64
                    ",\"vol_acc\":%u,\"vol_ref\":%u,\"idx_ref\":%d"
                    ",\"last_upd\":%" PRId64 ",\"fee\":%" PRIu64
                    ",\"protocol_fee\":%" PRIu64 ",\"touched\":[",
                    sig, pk, u->state_version,
                    m->auth_slot[pool_idx], m->auth_ns[pool_idx],
                    pool_idx, (unsigned)nix->protocol,
                    (unsigned)trig->dlmm.swap_for_y, trig->dlmm.amount_in,
                    ov->ix_exact ? 1u : 0u, ov->tx_exact ? 1u : 0u,
                    (unsigned)ov->klass, (unsigned)ov->n_leg,
                    (unsigned)ov->n_unknown,
                    active, act_after, rx, ry, vacc, vref, iref, last,
                    fee, pfee);
            for (bi = 0; bi < s_prime_d.nbin && bi < DLMM_BINS_MAX; bi++) {
                int32_t id = s_prime_d.bins[bi].id;
                uint64_t bx = s_prime_d.bins[bi].amount_x;
                uint64_t by = s_prime_d.bins[bi].amount_y;
                int changed = 1;
                if (h != NULL) {
                    uint32_t wi;
                    for (wi = 0; wi < DLMM_CACHE_WINDOW; wi++) {
                        if (h->live[wi] && h->bin[wi].id == id
                            && h->bin[wi].amount_x == bx
                            && h->bin[wi].amount_y == by) {
                            changed = 0;
                            break;
                        }
                    }
                }
                if (!changed) {
                    continue;
                }
                fprintf(af, "%s{\"id\":%d,\"x\":%" PRIu64 ",\"y\":%" PRIu64 "}",
                        first_bin ? "" : ",", id, bx, by);
                first_bin = 0;
            }
            fprintf(af, "]}");
        } else if (h != NULL
            && dlmm_cache_predict(&u->dlmm, pool_idx, &trig->dlmm, &spec, &res) == 0) {
            dlmm_hot_to_state(&spec, &s_prime_d);
            ds = &s_prime_d;
            active = spec.active_id;
            rx = spec.reserve_x;
            ry = spec.reserve_y;
            vacc = spec.v_parameters.volatility_accumulator;
            vref = spec.v_parameters.volatility_reference;
            iref = spec.v_parameters.index_reference;
            last = spec.v_parameters.last_update_timestamp;
            fee = res.fee;
            pfee = res.protocol_fee;
            act_after = res.active_id_after;
            if (partner != UINT32_MAX && partner < u->n && u->pump_live[partner]) {
                ps = &u->pump[partner];
            }
            fprintf(af,
                    "{\"kind\":\"opp_synced\",\"sig_hex\":\"%s\",\"pool\":\"%s\""
                    ",\"state_version_before\":%" PRIu64
                    ",\"auth_slot\":%" PRIu64 ",\"auth_ns\":%" PRIu64
                    ",\"n\":{\"pool_idx\":%u,\"proto\":%u,\"direction\":%u,"
                    "\"amount_in\":%" PRIu64 "}"
                    ",\"ix_exact\":%u,\"tx_exact\":%u"
                    ",\"s_prime\":{\"active_id\":%d,\"active_after\":%d"
                    ",\"reserve_x\":%" PRIu64 ",\"reserve_y\":%" PRIu64
                    ",\"vol_acc\":%u,\"vol_ref\":%u,\"idx_ref\":%d"
                    ",\"last_upd\":%" PRId64 ",\"fee\":%" PRIu64
                    ",\"protocol_fee\":%" PRIu64 ",\"touched\":[",
                    sig, pk, u->state_version,
                    m->auth_slot[pool_idx], m->auth_ns[pool_idx],
                    pool_idx, (unsigned)nix->protocol,
                    (unsigned)trig->dlmm.swap_for_y, trig->dlmm.amount_in,
                    1u, 0u,
                    active, act_after, rx, ry, vacc, vref, iref, last,
                    fee, pfee);
            for (i = 0; i < DLMM_CACHE_WINDOW; i++) {
                if (!spec.live[i]) {
                    continue;
                }
                if (h->live[i] && h->bin[i].amount_x == spec.bin[i].amount_x
                    && h->bin[i].amount_y == spec.bin[i].amount_y
                    && h->bin[i].id == spec.bin[i].id) {
                    continue;
                }
                fprintf(af, "%s{\"id\":%d,\"x\":%" PRIu64 ",\"y\":%" PRIu64 "}",
                        first_bin ? "" : ",", spec.bin[i].id,
                        spec.bin[i].amount_x, spec.bin[i].amount_y);
                first_bin = 0;
                (void)crossed;
            }
            fprintf(af, "]}");
        } else {
            fprintf(af,
                    "{\"kind\":\"opp_synced\",\"sig_hex\":\"%s\",\"pool\":\"%s\""
                    ",\"state_version_before\":%" PRIu64
                    ",\"n\":{\"pool_idx\":%u,\"proto\":%u,\"amount_in\":%" PRIu64 "}"
                    ",\"ix_exact\":%u,\"tx_exact\":0,\"s_prime\":null",
                    sig, pk, u->state_version, pool_idx,
                    (unsigned)nix->protocol, nix->amount_in, 1u);
        }
    } else if (ov != NULL && ov->tx_exact && ov->have_pump) {
        const dlmm_pool_hot_t *h;
        s_prime_p = ov->s_pump;
        ps = &s_prime_p;
        if (partner != UINT32_MAX && partner < u->n) {
            h = dlmm_cache_get(&u->dlmm, partner);
            if (h != NULL) {
                dlmm_hot_to_state(h, &s_dlmm);
                ds = &s_dlmm;
                active = h->active_id;
                rx = h->reserve_x;
                ry = h->reserve_y;
                vacc = h->v_parameters.volatility_accumulator;
                vref = h->v_parameters.volatility_reference;
                iref = h->v_parameters.index_reference;
                last = h->v_parameters.last_update_timestamp;
            }
        }
        fprintf(af,
                "{\"kind\":\"opp_synced\",\"sig_hex\":\"%s\",\"pool\":\"%s\""
                ",\"state_version_before\":%" PRIu64
                ",\"auth_slot\":%" PRIu64 ",\"auth_ns\":%" PRIu64
                ",\"n\":{\"pool_idx\":%u,\"proto\":%u,\"direction\":%u,"
                "\"amount_in\":%" PRIu64 "}"
                ",\"ix_exact\":1,\"tx_exact\":1,\"txo_klass\":%u"
                ",\"s_prime\":{\"kind\":\"pump\",\"reserve_base\":%" PRIu64
                ",\"reserve_quote\":%" PRIu64 ",\"partner_active\":%d"
                ",\"partner_rx\":%" PRIu64 ",\"partner_ry\":%" PRIu64
                ",\"vol_acc\":%u,\"vol_ref\":%u,\"idx_ref\":%d"
                ",\"last_upd\":%" PRId64 "}",
                sig, pk, u->state_version,
                m->auth_slot[pool_idx], m->auth_ns[pool_idx],
                pool_idx, (unsigned)nix->protocol,
                (unsigned)trig->pump.direction, trig->pump.amount_in,
                (unsigned)ov->klass,
                s_prime_p.reserve_base, s_prime_p.reserve_quote,
                active, rx, ry, vacc, vref, iref, last);
    } else if (u->meta[pool_idx].protocol == PROTO_PUMP && trig != NULL
               && u->pump_live[pool_idx]
               && pump_apply_swap(&u->pump[pool_idx], &trig->pump,
                                  &s_prime_p, &(pump_swap_result_t){0}) == 0) {
        const dlmm_pool_hot_t *h;
        ps = &s_prime_p;
        if (partner != UINT32_MAX && partner < u->n) {
            h = dlmm_cache_get(&u->dlmm, partner);
            if (h != NULL) {
                dlmm_hot_to_state(h, &s_dlmm);
                ds = &s_dlmm;
                active = h->active_id;
                rx = h->reserve_x;
                ry = h->reserve_y;
                vacc = h->v_parameters.volatility_accumulator;
                vref = h->v_parameters.volatility_reference;
                iref = h->v_parameters.index_reference;
                last = h->v_parameters.last_update_timestamp;
            }
        }
        fprintf(af,
                "{\"kind\":\"opp_synced\",\"sig_hex\":\"%s\",\"pool\":\"%s\""
                ",\"state_version_before\":%" PRIu64
                ",\"auth_slot\":%" PRIu64 ",\"auth_ns\":%" PRIu64
                ",\"n\":{\"pool_idx\":%u,\"proto\":%u,\"direction\":%u,"
                "\"amount_in\":%" PRIu64 "}"
                ",\"ix_exact\":1,\"tx_exact\":0"
                ",\"s_prime\":{\"kind\":\"pump\",\"reserve_base\":%" PRIu64
                ",\"reserve_quote\":%" PRIu64 ",\"partner_active\":%d"
                ",\"partner_rx\":%" PRIu64 ",\"partner_ry\":%" PRIu64
                ",\"vol_acc\":%u,\"vol_ref\":%u,\"idx_ref\":%d"
                ",\"last_upd\":%" PRId64 "}",
                sig, pk, u->state_version,
                m->auth_slot[pool_idx], m->auth_ns[pool_idx],
                pool_idx, (unsigned)nix->protocol,
                (unsigned)trig->pump.direction, trig->pump.amount_in,
                s_prime_p.reserve_base, s_prime_p.reserve_quote,
                active, rx, ry, vacc, vref, iref, last);
    } else {
        fprintf(af,
                "{\"kind\":\"opp_synced\",\"sig_hex\":\"%s\",\"pool\":\"%s\""
                ",\"state_version_before\":%" PRIu64
                ",\"n\":{\"pool_idx\":%u,\"proto\":%u,\"amount_in\":%" PRIu64 "}"
                ",\"ix_exact\":1,\"tx_exact\":0",
                sig, pk, u->state_version, pool_idx,
                (unsigned)nix->protocol, nix->amount_in);
    }
    if (ds != NULL && ps != NULL && opp->amount_in != 0) {
        mid = cycle_mid(ds, ps, opp->amount_in, opp->direction);
    }
    audit_send_quote(af, nix, sv, ds, ps, opp->direction);
    fprintf(af,
            ",\"arb\":{\"direction\":%u,\"amount_in\":%" PRIu64
            ",\"intermediate\":%" PRIu64 ",\"final\":%" PRIu64
            ",\"gross\":%" PRIu64 "}"
            ",\"frame\":{\"class\":\"framed\",\"t0_ns\":%" PRIu64
            ",\"framed_ns\":%" PRIu64 ",\"delay_ns\":%" PRIu64
            ",\"nsig\":%u,\"nkeys\":%u,\"ninstr\":%u}"
            ",\"fresh\":{\"auth_slot\":%" PRIu64 ",\"shred_slot\":%" PRIu64
            ",\"age_slots\":%" PRIu64 ",\"max_slots\":%" PRIu64
            ",\"prior_mut_slot\":%" PRIu64 ",\"ok\":%u}"
            ",\"race_ready\":%u"
            ",\"auth_generation\":%" PRIu64
            ",\"auth_tx_sig\":\"%s\""
            ",\"auth_age_slots\":%" PRIu64
            ",\"state_before_hash\":\"%016" PRIx64 "\""
            ",\"predicted_after_hash\":\"%016" PRIx64 "\""
            ",\"timing\":{\"actionable_ns\":%" PRIu64
            ",\"decision_ns\":%u,\"signed_ready_ns\":%u}}\n",
            (unsigned)opp->direction, opp->amount_in, mid,
            opp->amount_out, opp->gross_profit,
            fi != NULL ? fi->t0_ns : 0,
            fi != NULL ? fi->framed_ns : 0,
            (fi != NULL && fi->framed_ns >= fi->t0_ns)
                ? (fi->framed_ns - fi->t0_ns) : 0,
            fi != NULL ? (unsigned)fi->nsig : 0u,
            fi != NULL ? (unsigned)fi->nkeys : 0u,
            fi != NULL ? (unsigned)fi->ninstr : 0u,
            m->auth_slot[pool_idx],
            sv != NULL ? sv->slot : (uint64_t)0,
            (sv != NULL && sv->slot >= m->auth_slot[pool_idx])
                ? (sv->slot - m->auth_slot[pool_idx]) : (uint64_t)0,
            FRESH_MAX_SLOTS, prior_mut, race_ready ? 1u : 0u,
            race_ready ? 1u : 0u,
            am != NULL ? am->generation : 0,
            asig,
            (sv != NULL && am != NULL && sv->slot >= am->slot)
                ? (sv->slot - am->slot) : (uint64_t)0,
            am != NULL ? am->hash : 0,
            authpub_hash(&s_prime_d, sizeof(s_prime_d))
                ^ authpub_hash(&s_prime_p, sizeof(s_prime_p)),
            actionable_ns, decide_ns, signed_ns);
    fflush(af);
}

static const char *
proto_name(uint8_t p)
{
    if (p == PROTO_DLMM) {
        return "dlmm";
    }
    if (p == PROTO_PUMP) {
        return "pump";
    }
    if (p == PROTO_CPMM) {
        return "cpmm";
    }
    return "x";
}

static void
write_gate(FILE *af, uint64_t ts, int pool_idx, unsigned proto,
           int known, int routed, int searchable,
           uint64_t gross, int gross_pos,
           uint64_t cap_gross, int cap_pos, int cap_hurdle,
           unsigned n_hop, unsigned n_sync, int synced_all,
           uint64_t age_max, int age32_all, int mut_ok_all,
           int mut_authoritative, int would_send_new, int s007_ready,
           int updating, uint64_t state_gen, uint64_t last_auth_slot,
           uint64_t last_rx_ns, unsigned family, const char *seq,
           int exec_fam, uint64_t shred_slot)
{
    if (af == NULL) {
        return;
    }
    fprintf(af,
            "{\"kind\":\"gate\",\"ts\":%" PRIu64
            ",\"pool_idx\":%d,\"proto\":%u"
            ",\"known\":%u,\"routed\":%u,\"searchable\":%u"
            ",\"gross\":%" PRIu64 ",\"gross_pos\":%u"
            ",\"cap_gross\":%" PRIu64 ",\"cap_pos\":%u,\"cap_hurdle\":%u"
            ",\"n_hop\":%u,\"n_sync\":%u,\"synced_all\":%u"
            ",\"age_max\":%" PRIu64 ",\"age32_all\":%u,\"mut_ok_all\":%u"
            ",\"mut_authoritative\":%u,\"would_send_new\":%u"
            ",\"s007_ready\":%u,\"updating\":%u"
            ",\"state_gen\":%" PRIu64 ",\"last_auth_slot\":%" PRIu64
            ",\"last_rx_ns\":%" PRIu64
            ",\"family\":%u,\"seq\":\"%s\",\"exec_fam\":%u"
            ",\"shred_slot\":%" PRIu64 "}\n",
            ts, pool_idx, proto,
            known ? 1u : 0u, routed ? 1u : 0u, searchable ? 1u : 0u,
            gross, gross_pos ? 1u : 0u,
            cap_gross, cap_pos ? 1u : 0u, cap_hurdle ? 1u : 0u,
            n_hop, n_sync, synced_all ? 1u : 0u,
            age_max, age32_all ? 1u : 0u, mut_ok_all ? 1u : 0u,
            mut_authoritative ? 1u : 0u, would_send_new ? 1u : 0u,
            s007_ready ? 1u : 0u, updating ? 1u : 0u,
            state_gen, last_auth_slot, last_rx_ns,
            family, seq != NULL ? seq : "",
            exec_fam ? 1u : 0u, shred_slot);
    fflush(af);
}

static void
scan_route_fresh(const race_ctx_t *cx, uint32_t route_id, uint64_t shred,
                 unsigned *n_hop, unsigned *n_sync, unsigned *n_age,
                 unsigned *n_mut, unsigned *n_mut_auth, uint64_t *age_max)
{
    const compiled_route_t *cr;
    uint8_t h;

    *n_hop = 0;
    *n_sync = 0;
    *n_age = 0;
    *n_mut = 0;
    *n_mut_auth = 0;
    *age_max = 0;
    if (cx == NULL || cx->u == NULL || route_id >= cx->u->routes.n_route) {
        return;
    }
    cr = &cx->u->routes.route[route_id];
    *n_hop = cr->n_hop;
    for (h = 0; h < cr->n_hop; h++) {
        uint32_t p = cr->pool[h];
        uint64_t auth = 0;
        uint64_t age = ~0ull;
        uint64_t prior = 0;
        if (!POOL_OK(p)) {
            continue;
        }
        if (cx->smgr != NULL) {
            auth = cx->smgr->auth_slot[p];
            if (state_sendable(cx->smgr, cx->u, p)
                && cx->smgr->auth_ns[p] != 0) {
                (*n_sync)++;
            }
        }
        prior = g_mut_slot[p];
        if (auth != 0 && shred >= auth) {
            age = shred - auth;
            if (age > *age_max) {
                *age_max = age;
            }
            if (age <= FRESH_MAX_SLOTS) {
                (*n_age)++;
            }
        }
        if (auth != 0 && prior <= auth) {
            (*n_mut)++;
        }
        if (g_s007_ready
            && g_s007_updating[p] == 0
            && cx->smgr != NULL
            && state_sendable(cx->smgr, cx->u, p)
            && cx->smgr->auth_ns[p] != 0) {
            (*n_mut_auth)++;
        }
    }
}

static void
note_searchable(race_ctx_t *cx, uint32_t pool_idx, const hot_trigger_t *trig,
                const swapix_t *nix, const shred_view_t *sv)
{
    paper_opp_t pop;
    const char *bucket = "";
    const char *seq = "";
    char seqbuf[48];
    uint64_t cap_out = 0;
    uint64_t cap_gross = 0;
    uint64_t gross = 0;
    uint64_t shred_slot = sv != NULL ? sv->slot : 0;
    uint64_t auth = 0;
    uint64_t age = ~0ull;
    uint64_t age_max = 0;
    uint64_t ts = now_realtime_ns() / 1000000000ull;
    unsigned n_hop = 0, n_sync = 0, n_age = 0, n_mut = 0, n_mut_auth = 0;
    unsigned family = 255u;
    int routed = 0;
    int searchable = 0;
    int cap_ok = 0;
    int cap_hurdle = 0;
    int gross_pos = 0;
    int age_ok = 0;
    int sendable = 0;
    int synced_all = 0;
    int age32_all = 0;
    int mut_ok_all = 0;
    int mut_authoritative = 0;
    int would_send_new = 0;
    int exec_fam = 0;
    int updating = 0;
    uint64_t state_gen = 0;
    uint64_t last_rx_ns = 0;

    if (cx == NULL || cx->u == NULL) {
        return;
    }
    memset(&pop, 0, sizeof(pop));
    routed = (pool_idx < cx->u->routes.n_pool
              && cx->u->routes.pool[pool_idx].route_n > 0);
    if (trig != NULL && cx->st != NULL
        && paper_eval_trigger(cx->u, cx->st, pool_idx, trig, &pop) == 0
        && pop.opp.valid) {
        searchable = 1;
        family = pop.family;
        n_hop = pop.n_hop;
        gross = pop.opp.gross_profit;
        gross_pos = gross > 0;
        snprintf(seqbuf, sizeof(seqbuf), "%s%s%s%s%s",
                 pop.n_hop >= 1 ? proto_name(pop.proto[0]) : "",
                 pop.n_hop >= 2 ? "-" : "",
                 pop.n_hop >= 2 ? proto_name(pop.proto[1]) : "",
                 pop.n_hop >= 3 ? "-" : "",
                 pop.n_hop >= 3 ? proto_name(pop.proto[2]) : "");
        seq = seqbuf;
        if (pop.n_hop >= 3) {
            cx->acc->fun_hop3++;
            bucket = "3hop";
        } else if (pop.family == ROUTE_FAM_5_DLMM_PUMP_TYPED) {
            cx->acc->fun_fam5++;
            bucket = "fam5";
        } else if (pop.family == ROUTE_FAM_0_DLMM_PUMP) {
            cx->acc->fun_ev0++;
            bucket = "route0";
        } else if (pop.family == ROUTE_FAM_6_SUPPORTED) {
            bucket = "supported";
        } else {
            cx->acc->fun_state++;
            bucket = "other";
        }
        if (paper_quote_route(cx->u, cx->st, pop.route_id, 50000000ull,
                              &cap_out) == 0
            && cap_out > 50000000ull) {
            cap_ok = 1;
            cap_gross = cap_out - 50000000ull;
            cap_hurdle = cap_gross > hops_hurdle_for_seq(seq);
        }
        scan_route_fresh(cx, pop.route_id, shred_slot,
                         &n_hop, &n_sync, &n_age, &n_mut, &n_mut_auth,
                         &age_max);
        synced_all = (n_hop > 0 && n_sync == n_hop);
        age32_all = (n_hop > 0 && n_age == n_hop);
        mut_ok_all = (n_hop > 0 && n_mut == n_hop);
        mut_authoritative = (n_hop > 0 && n_mut_auth == n_hop);
        exec_fam = (pop.family == ROUTE_FAM_0_DLMM_PUMP
                    || pop.family == ROUTE_FAM_5_DLMM_PUMP_TYPED
                    || pop.family == ROUTE_FAM_6_SUPPORTED);
        would_send_new = cap_hurdle && mut_authoritative;
    } else {
        cx->acc->fun_state++;
    }
    if (cx->smgr != NULL && POOL_OK(pool_idx)) {
        auth = cx->smgr->auth_slot[pool_idx];
        age = (shred_slot >= auth) ? (shred_slot - auth) : ~0ull;
        age_ok = (auth != 0 && shred_slot >= auth && age <= FRESH_MAX_SLOTS);
        sendable = state_sendable(cx->smgr, cx->u, pool_idx)
            && cx->smgr->auth_ns[pool_idx] != 0;
        updating = g_s007_updating[pool_idx];
        state_gen = g_s007_gen[pool_idx];
        last_rx_ns = g_s007_rx_ns[pool_idx];
    }
    if (age32_all) {
        cx->acc->fun_age32++;
    }
    if (mut_authoritative) {
        cx->acc->fun_mut_auth++;
    }
    if (would_send_new) {
        cx->acc->fun_would_new++;
    }
    if (updating) {
        cx->acc->fun_updating++;
    }
    write_gate(cx->auditf, ts, (int)pool_idx,
               nix != NULL ? (unsigned)nix->protocol : 0u,
               1, routed, searchable, gross, gross_pos, cap_gross, cap_ok,
               cap_hurdle, n_hop, n_sync, synced_all, age_max, age32_all,
               mut_ok_all, mut_authoritative, would_send_new,
               (int)g_s007_ready, updating, state_gen, auth, last_rx_ns,
               family, seq, exec_fam, shred_slot);
    if (!searchable || cx->auditf == NULL || nix == NULL) {
        return;
    }
    {
        char sig[129];
        hex_encode(nix->sig, 64, sig);
        fprintf(cx->auditf,
                "{\"kind\":\"opp_searchable\",\"bucket\":\"%s\",\"family\":%u"
                ",\"n_hop\":%u,\"seq\":\"%s\",\"pool_idx\":%u"
                ",\"gross\":%" PRIu64 ",\"amount_in\":%" PRIu64
                ",\"cap_ok\":%u,\"cap_gross\":%" PRIu64
                ",\"fresh\":{\"auth_slot\":%" PRIu64 ",\"shred_slot\":%" PRIu64
                ",\"age_slots\":%" PRIu64 ",\"ok\":%u,\"sendable\":%u}"
                ",\"reason\":%u,\"sig_hex\":\"%s\"}\n",
                bucket, family, n_hop, seq, pool_idx, gross,
                pop.opp.amount_in, cap_ok ? 1u : 0u, cap_gross, auth,
                shred_slot, age == ~0ull ? (uint64_t)0 : age,
                age_ok ? 1u : 0u, sendable ? 1u : 0u,
                (unsigned)pop.reason, sig);
        fflush(cx->auditf);
    }
}

static void
consider_framed(race_ctx_t *cx, swapix_t *nix, hot_trigger_t *trig,
                const shred_view_t *sv, const frame_info_t *fi,
                uint64_t rx_ns, uint64_t c0,
                const uint8_t *txp, uint32_t txlen)
{
    hot_decision_t dec;
    affected_pool_t hit;
    uint64_t c1;
    tx_overlay_t ov;

    if (cx == NULL || nix == NULL || trig == NULL) {
        return;
    }
    if (pool_table_get(&cx->u->keys, nix->pool, &hit) != 0) {
        cx->acc->unknown++;
        journal_discover(nix->pool, nix->protocol);
        if (cx->auditf != NULL) {
            char pk[65], sig[129];
            hex_encode(nix->pool, 32, pk);
            hex_encode(nix->sig, 64, sig);
            fprintf(cx->auditf,
                    "{\"kind\":\"unknown_pool\",\"pool_hex\":\"%s\",\"proto\":%u"
                    ",\"sig_hex\":\"%s\"}\n",
                    pk, (unsigned)nix->protocol, sig);
            fflush(cx->auditf);
            write_gate(cx->auditf, now_realtime_ns() / 1000000000ull, -1,
                       (unsigned)nix->protocol, 0, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, 0, 0, (int)g_s007_ready, 0, 0, 0, 0,
                       255u, "", 0, sv != NULL ? sv->slot : 0);
        }
        return;
    }
    cx->acc->known++;
    if (hit.pool_idx < cx->u->routes.n_pool
        && cx->u->routes.pool[hit.pool_idx].route_n > 0) {
        cx->acc->searchable++;
    }
    cx->acc->decide++;
    pend_put(cx->pq, nix->sig, hit.pool_idx, trig);
    pend_log(cx->pq, nix->sig, hit.pool_idx, nix->protocol);
    {
        uint64_t trig_slot = sv != NULL ? sv->slot : 0;
        uint32_t partner = UINT32_MAX;
        uint8_t want = (cx->u->meta[hit.pool_idx].protocol == PROTO_DLMM)
            ? PROTO_PUMP : PROTO_DLMM;
        memset(&cx->last_auth, 0, sizeof(cx->last_auth));
        if (install_auth(cx, hit.pool_idx, trig_slot, nix->sig) != 0) {
            cx->acc->auth_pub_miss++;
            cx->acc->missing_state++;
            state_mark_stale(cx->smgr, hit.pool_idx);
            note_searchable(cx, hit.pool_idx, trig, nix, sv);
            return;
        }
        {
            authpub_meta_t trig_auth = cx->last_auth;
            if (find_route0_partner(cx->u, hit.pool_idx, want, &partner) == 0
                && partner < cx->u->n
                && install_auth(cx, partner, trig_slot, nix->sig) != 0) {
                cx->acc->auth_pub_miss++;
                cx->acc->missing_state++;
                state_mark_stale(cx->smgr, partner);
                note_searchable(cx, hit.pool_idx, trig, nix, sv);
                return;
            }
            cx->last_auth = trig_auth;
        }
        cx->acc->auth_pub_ok++;
        if (authpub_ready()) {
            g_s007_ready = 1;
        }
    }
    memset(&ov, 0, sizeof(ov));
    ov.ix_exact = 1;
    ov.tx_exact = 0;
    ov.klass = TXO_IX_ONLY;
    if (txp != NULL && txlen >= 65u && txo_scan(txp, txlen, &ov) == 0) {
        txo_bind_watched(&ov, nix->pool);
        txo_stamp_n(&ov, nix);
        if (ov.tx_exact && ov.klass == TXO_EXACT) {
            if (nix->protocol == PROTO_DLMM) {
                const dlmm_pool_hot_t *h = dlmm_cache_get(&cx->u->dlmm, hit.pool_idx);
                dlmm_state_t before;
                if (h != NULL) {
                    dlmm_hot_to_state(h, &before);
                    if (txo_apply(&ov, nix->pool, PROTO_DLMM, &before, NULL) != 0) {
                        ov.tx_exact = 0;
                    }
                } else {
                    ov.tx_exact = 0;
                }
            } else if (nix->protocol == PROTO_PUMP
                       && hit.pool_idx < cx->u->n
                       && cx->u->pump_live[hit.pool_idx]) {
                if (txo_apply(&ov, nix->pool, PROTO_PUMP, NULL,
                              &cx->u->pump[hit.pool_idx]) != 0) {
                    ov.tx_exact = 0;
                }
            } else {
                ov.tx_exact = 0;
            }
        }
    } else {
        ov.ix_exact = 1;
        ov.tx_exact = 0;
        ov.klass = TXO_IX_ONLY;
    }
    if (ov.tx_exact) {
        cx->acc->tx_exact++;
    } else if (ov.ix_exact) {
        cx->acc->ix_exact++;
    }
    {
        int st = state_observe(cx->smgr, cx->u, hit.pool_idx, trig, &dec);
        if (st == ST3_MISSING_BIN || st == ST3_APPLY_FAIL
            || st == ST3_UNHEALTHY || st < 0) {
            cx->acc->missing_state++;
            if (st == ST3_MISSING_BIN || st < 0) {
                note_searchable(cx, hit.pool_idx, trig, nix, sv);
                return;
            }
        } else {
            cx->acc->state_ok++;
        }
    }
    note_searchable(cx, hit.pool_idx, trig, nix, sv);
    (void)syncrec_decision(cx->sync, rx_ns, nix, hit.pool_idx,
                           dec.state_version, nix->amount_in,
                           nix->min_out, &dec.opp);
    if (hot_stale(cx->u, dec.state_version)) {
        cx->acc->stale++;
    }
    c1 = rdtscp();
    if (cx->acc->n_act < LAT_MAX) {
        cx->acc->act[cx->acc->n_act++] =
            (uint32_t)cyc_ns(cx->clk, c1 - c0);
    }
    if ((cx->acc->shred & 31ull) == 0ull) {
        recon_drain(cx->pq, cx->u, cx->smgr);
    }
    if (!dec.opp.valid) {
        return;
    }
    cx->acc->opp++;
    {
        uint64_t shred_slot = sv != NULL ? sv->slot : 0;
        uint64_t auth = cx->smgr->auth_slot[hit.pool_idx];
        uint64_t prior = (POOL_OK(hit.pool_idx))
            ? g_mut_slot[hit.pool_idx] : 0;
        uint64_t age = (shred_slot >= auth) ? (shred_slot - auth) : ~0ull;
        int age_ok = (auth != 0 && shred_slot >= auth && age <= FRESH_MAX_SLOTS);
        int mut_ok = (prior <= auth);
        int mut_auth = 0;
        int race_ready = 0;

        if (POOL_OK(hit.pool_idx) && shred_slot > g_mut_slot[hit.pool_idx]) {
            g_mut_slot[hit.pool_idx] = shred_slot;
        }
        if (state_sendable(cx->smgr, cx->u, hit.pool_idx)
            && cx->smgr->auth_ns[hit.pool_idx] != 0) {
            uint32_t signed_ns = 0;
            mut_auth = (g_s007_ready
                        && POOL_OK(hit.pool_idx)
                        && g_s007_updating[hit.pool_idx] == 0);
            if (mut_auth) {
                uint32_t partner = UINT32_MAX;
                uint8_t want = (cx->u->meta[hit.pool_idx].protocol == PROTO_DLMM)
                    ? PROTO_PUMP : PROTO_DLMM;
                if (find_route0_partner(cx->u, hit.pool_idx, want, &partner) == 0
                    && POOL_OK(partner)) {
                    if (g_s007_updating[partner]
                        || !state_sendable(cx->smgr, cx->u, partner)
                        || cx->smgr->auth_ns[partner] == 0) {
                        mut_auth = 0;
                    }
                }
            }
            /* Production freshness is mutation-authoritative. age32 is journaled only.
             * Funded eligibility: TX_EXACT + mut_authoritative + route RACE_READY.
             * IX_EXACT is paper-observable but not fundable. FUNDED stays 0. */
            if (!mut_auth) {
                cx->acc->fresh_drop++;
                if (cx->auditf != NULL) {
                    fprintf(cx->auditf,
                            "{\"kind\":\"fresh_drop\",\"pool_idx\":%u"
                            ",\"auth_slot\":%" PRIu64 ",\"shred_slot\":%" PRIu64
                            ",\"age_slots\":%" PRIu64 ",\"prior_mut_slot\":%" PRIu64
                            ",\"age_ok\":%u,\"mut_ok\":%u,\"mut_authoritative\":%u"
                            ",\"s007_ready\":%u}\n",
                            hit.pool_idx, auth, shred_slot, age, prior,
                            age_ok ? 1u : 0u, mut_ok ? 1u : 0u,
                            mut_auth ? 1u : 0u, (unsigned)g_s007_ready);
                    fflush(cx->auditf);
                }
                return;
            }
            cx->acc->opp_synced++;
            race_ready = ov.tx_exact;
            if (FUNDED && race_ready && dec.opp.route_id == ROUTE_DLMM_PUMP) {
                uint8_t tx[V0_TX_LEN];
                uint8_t bh[32];
                memset(bh, 0xBB, 32);
                if (route0_v0_patch(cx->v0, &dec.opp, 1, bh, 1, tx) == 0
                    && route0_v0_sign(cx->signer, tx) == 0) {
                    cx->acc->signed_ready++;
                    signed_ns = (uint32_t)cyc_ns(cx->clk, rdtscp() - c0);
                    if (cx->acc->n_sgn < LAT_MAX) {
                        cx->acc->sgn[cx->acc->n_sgn++] = signed_ns;
                    }
                    (void)syncrec_exec(cx->sync, rx_ns, nix->sig,
                                       signed_ns, 0, SYN_EXEC_READY);
                }
            }
            audit_opp_synced(cx->auditf, cx->u, cx->smgr, nix, hit.pool_idx,
                             trig, &dec.opp, sv, fi, rx_ns,
                             (uint32_t)cyc_ns(cx->clk, c1 - c0), signed_ns,
                             prior, race_ready, &cx->last_auth, &ov);
        }
    }
}

static void
note_framed(acc_t *acc, const frame_info_t *fi)
{
    uint64_t delay;
    if (acc == NULL || fi == NULL) {
        return;
    }
    acc->frame_framed++;
    delay = (fi->framed_ns >= fi->t0_ns) ? (fi->framed_ns - fi->t0_ns) : 0;
    if (acc->n_fdelay < LAT_MAX && acc->fdelay != NULL) {
        acc->fdelay[acc->n_fdelay++] =
            (uint32_t)(delay > 0xffffffffull ? 0xffffffffu : delay);
    }
}

static int
close_candidate(race_ctx_t *cx, fslot_t *fs, swapix_t *nix,
                hot_trigger_t *trig, const shred_view_t *sv,
                uint64_t t0_ns, uint32_t around, uint64_t rx_ns, uint64_t c0,
                int from_hold)
{
    frame_hit_t hit;
    frame_info_t fi;
    uint32_t n;
    int klass;

    n = frame_concat(fs, around);
    if (n < 65u) {
        klass = FRAME_INCOMPLETE;
        memset(&hit, 0, sizeof(hit));
    } else {
        klass = frame_around_sig(g_fbuf, n, nix->sig, &hit);
    }
    if (klass == FRAME_INVALID) {
        cx->acc->frame_invalid++;
        audit_frame(cx->auditf, nix->sig, sv, FRAME_INVALID, t0_ns,
                    now_ns(), &hit);
        return FRAME_INVALID;
    }
    if (klass != FRAME_FRAMED) {
        if (!from_hold) {
            cx->acc->frame_incomplete++;
            hold_put(nix->sig, sv->slot, around, t0_ns, nix, trig);
            audit_frame(cx->auditf, nix->sig, sv, FRAME_INCOMPLETE, t0_ns,
                        now_ns(), &hit);
        }
        return FRAME_INCOMPLETE;
    }
    memset(&fi, 0, sizeof(fi));
    fi.nsig = hit.nsig;
    fi.nkeys = hit.nkeys;
    fi.ninstr = hit.ninstr;
    fi.t0_ns = t0_ns;
    fi.framed_ns = now_ns();
    note_framed(cx->acc, &fi);
    audit_frame(cx->auditf, nix->sig, sv, FRAME_FRAMED, t0_ns,
                fi.framed_ns, &hit);
    {
        trigger_hit_t admitted;
        /* All paths, including static-program discovery and held fragments,
         * pass here before installing AUTH or applying any prediction. */
        alt_bank_t bank = {.slot = sv->slot};
        uint64_t decision_ns = now_ns();
        int result = trigger_admit_at(g_fbuf + hit.tx_off, hit.tx_end - hit.tx_off,
                                     nix->sig, cx->alts, cx->watch, &bank, &admitted);
        int accepted = result == TRIG_EXACT && swapix_to_trigger(&admitted.exact, trig) == 0;
        if (cx->auditf != NULL) {
            char sig[129];
            hex_encode(nix->sig, 64, sig);
            fprintf(cx->auditf,
                    "{\"kind\":\"raw_admission\",\"sig_hex\":\"%s\",\"slot\":%" PRIu64
                    ",\"raw_rx_ns\":%" PRIu64 ",\"decision_ns\":%" PRIu64
                    ",\"from_hold\":%u,\"accepted\":%u,\"klass\":%d,\"alt_status\":%u"
                    ",\"bank_id\":null,\"provider_generation\":null"
                    ",\"scope\":\"resolved_keys_and_one_swap\",\"quote_certified\":false}\n",
                    sig, sv->slot, t0_ns, decision_ns, (unsigned)(from_hold != 0),
                    (unsigned)accepted, result, (unsigned)admitted.alt_status);
            fflush(cx->auditf);
        }
        if (!accepted)
            return FRAME_FRAMED; /* Complete bytes; admission failed at this deadline. */
        *nix = admitted.exact;
    }
    if (cx->auditf != NULL) {
        char pk[65], sig[129];
        hex_encode(nix->pool, 32, pk);
        hex_encode(nix->sig, 64, sig);
        fprintf(cx->auditf,
                "{\"kind\":\"framed_pool\",\"pool_hex\":\"%s\",\"proto\":%u"
                ",\"sig_hex\":\"%s\"}\n",
                pk, (unsigned)nix->protocol, sig);
        fflush(cx->auditf);
    }
    {
        const uint8_t *txp = NULL;
        uint32_t txlen = 0;
        if (hit.tx_end > hit.tx_off && hit.tx_end <= n) {
            txp = g_fbuf + hit.tx_off;
            txlen = hit.tx_end - hit.tx_off;
        }
        consider_framed(cx, nix, trig, sv, &fi, rx_ns, c0, txp, txlen);
    }
    return FRAME_FRAMED;
}

static void
hold_retry_slot(race_ctx_t *cx, fslot_t *fs, const shred_view_t *sv,
                uint64_t rx_ns)
{
    uint32_t i;
    if (fs == NULL || sv == NULL) {
        return;
    }
    for (i = 0; i < HOLD_MAX; i++) {
        hold_ent_t *e = &g_hold[i];
        uint64_t c0;
        int klass;
        if (!e->used || e->slot != sv->slot) {
            continue;
        }
        c0 = rdtscp();
        klass = close_candidate(cx, fs, &e->nix, &e->trig, sv, e->t0_ns,
                                e->index, rx_ns, c0, 1);
        if (klass != FRAME_INCOMPLETE) {
            e->used = 0;
        }
    }
}

static void
telemetry(const acc_t *a, const live_univ_t *u, const state_mgr_t *m,
          uint64_t up, uint64_t rxps)
{
    fprintf(stderr,
            "PAPER-ORBIT  up=%" PRIu64 "s  rx=%" PRIu64 "/s  shred=%" PRIu64
            "  N=%" PRIu64 "  known=%" PRIu64 "  unknown=%" PRIu64
            "  decide=%" PRIu64
            "  opp=%" PRIu64 "  opp_synced=%" PRIu64
            "  both=%" PRIu64 "  ver=%" PRIu64
            "  frame inv=%" PRIu64 " hold=%" PRIu64 " framed=%" PRIu64
            " exp=%" PRIu64 " fresh_drop=%" PRIu64 "\n",
            up, rxps, a->shred, a->decoded, a->known, a->unknown, a->decide, a->opp,
            a->opp_synced, a->both, u->state_version,
            a->frame_invalid, a->frame_incomplete, a->frame_framed,
            a->frame_expired, a->fresh_drop);
    {
        uint64_t fr = a->frame_framed;
        fprintf(stderr,
                "  funnel framed=%" PRIu64 " known=%" PRIu64 " (%.1f%%)"
                " searchable=%" PRIu64 " (%.1f%%)"
                " ev0=%" PRIu64 " fam5=%" PRIu64 " hop3=%" PRIu64
                " state=%" PRIu64 " age32=%" PRIu64
                " mut_auth=%" PRIu64 " would_new=%" PRIu64
                " updating=%" PRIu64 " s007=%u sendable=%" PRIu64
                " trig gen=%" PRIu64 " exact=%" PRIu64 " unk=%" PRIu64
                " alt_miss=%" PRIu64
                " auth_ok=%" PRIu64 " auth_miss=%" PRIu64
                " ix_exact=%" PRIu64 " tx_exact=%" PRIu64 "\n",
                fr, a->known,
                fr ? (100.0 * (double)a->known / (double)fr) : 0.0,
                a->searchable,
                fr ? (100.0 * (double)a->searchable / (double)fr) : 0.0,
                a->fun_ev0, a->fun_fam5, a->fun_hop3,
                a->fun_state, a->fun_age32,
                a->fun_mut_auth, a->fun_would_new, a->fun_updating,
                (unsigned)g_s007_ready, a->opp_synced,
                a->trig_generic, a->trig_exact, a->trig_unknown,
                a->trig_alt_miss, a->auth_pub_ok, a->auth_pub_miss,
                a->ix_exact, a->tx_exact);
    }
    state_metrics_print(m, stderr);
    fflush(stderr);
}

int
main(int argc, char **argv)
{
    live_univ_t *u;
    universe_gen_t *gen;
    alt_cache_t alts;
    trigger_metrics_t trigm;
    univ_state_t ust;
    route0_v0_template_t v0;
    route0_signer_t signer;
    acc_t acc;
    struct tsc_clock clk;
    syncrec_t sync;
    state_mgr_t smgr;
    hot_seen_t seen;
    fec_row_t *fec;
    flow_ent_t *flow;
    race_ctx_t rcx;
    const char *dir = NULL;
    const char *univ = NULL;
    const char *sync_path = NULL;
    const char *flow_idx = NULL;
    const char *flow_dir = NULL;
    const char *stats_path = NULL;
    const char *pend_path = NULL;
    const char *recon_path = NULL;
    const char *audit_path = NULL;
    FILE *auditf = NULL;
    pend_q_t pq;
    char idx_paths[32][1024];
    int n_idx = 0;
    uint64_t seconds = 1200;
    uint8_t seed[32];
    uint8_t sk[V0_N_STATIC * 32];
    uint8_t wr[V0_N_ALT_WR * 32];
    uint8_t ro[V0_N_ALT_RO * 32];
    uint8_t alt[32];
    char cap_path[1024];
    FILE *cf = NULL;
    feedcap_hdr_t hdr;
    uint64_t t0, last_tel, last_flow, flow_n = 0;
    uint64_t last_shred = 0;
    int start_at_eof = 1;
    int ai;

    memset(&acc, 0, sizeof(acc));
    gen = NULL;
    u = NULL;
    memset(&sync, 0, sizeof(sync));
    memset(&smgr, 0, sizeof(smgr));
    memset(&seen, 0, sizeof(seen));
    memset(&pq, 0, sizeof(pq));
    memset(seed, 0x44, 32);
    acc.lead_min = (int64_t)((uint64_t)1 << 62);
    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--dir") == 0 && ai + 1 < argc) {
            dir = argv[++ai];
        } else if (strcmp(argv[ai], "--univ") == 0 && ai + 1 < argc) {
            univ = argv[++ai];
        } else if (strcmp(argv[ai], "--sync") == 0 && ai + 1 < argc) {
            sync_path = argv[++ai];
        } else if (strcmp(argv[ai], "--flowra-idx") == 0 && ai + 1 < argc) {
            flow_idx = argv[++ai];
        } else if (strcmp(argv[ai], "--flowra-dir") == 0 && ai + 1 < argc) {
            flow_dir = argv[++ai];
        } else if (strcmp(argv[ai], "--stats") == 0 && ai + 1 < argc) {
            stats_path = argv[++ai];
        } else if (strcmp(argv[ai], "--seconds") == 0 && ai + 1 < argc) {
            seconds = strtoull(argv[++ai], NULL, 10);
        } else if (strcmp(argv[ai], "--pend") == 0 && ai + 1 < argc) {
            pend_path = argv[++ai];
        } else if (strcmp(argv[ai], "--recon") == 0 && ai + 1 < argc) {
            recon_path = argv[++ai];
        } else if (strcmp(argv[ai], "--audit") == 0 && ai + 1 < argc) {
            audit_path = argv[++ai];
        } else {
            fprintf(stderr, "usage: %s --dir CAPDIR --univ liveuniv.bin --sync sync.bin [--flowra-dir D] [--seconds N]\n",
                    argv[0]);
            return 1;
        }
    }
    if (dir == NULL || univ == NULL || sync_path == NULL) {
        fprintf(stderr, "need --dir --univ --sync\n");
        return 1;
    }
    acc.act = calloc(LAT_MAX, sizeof(*acc.act));
    acc.sgn = calloc(LAT_MAX, sizeof(*acc.sgn));
    acc.fdelay = calloc(LAT_MAX, sizeof(*acc.fdelay));
    fec = calloc(FEC_CAP, sizeof(*fec));
    flow = calloc(FLOW_CAP, sizeof(*flow));
    if (acc.act == NULL || acc.sgn == NULL || acc.fdelay == NULL
        || fec == NULL || flow == NULL) {
        return 1;
    }
    {
        uint32_t si;
        for (si = 0; si < F_SLOTS; si++) {
            g_fslot[si].arena = calloc(1, F_ARENA);
            if (g_fslot[si].arena == NULL) {
                return 1;
            }
        }
    }
    if (tsc_calibrate(&clk) != 0) {
        return 1;
    }
    gen = calloc(1, sizeof(*gen));
    if (gen == NULL) {
        return 1;
    }
    if (universe_gen_build(gen, univ, 1) != 0) {
        fprintf(stderr, "universe_gen build failed\n");
        return 1;
    }
    (void)universe_gen_publish(gen);
    u = &gen->univ;
    if (runtime_pools(u->n) != 0) {
        fprintf(stderr, "runtime pool alloc failed\n");
        return 1;
    }
    if (authpub_open(AUTH_PATH_DEFAULT) != 0) {
        fprintf(stderr, "AUTH-PUBLISH  missing %s  fail-closed until STATE-008\n",
                AUTH_PATH_DEFAULT);
    } else {
        fprintf(stderr, "AUTH-PUBLISH  %s ready=%d\n",
                AUTH_PATH_DEFAULT, authpub_ready());
    }
    memset(&ust, 0, sizeof(ust));
    if (univ_state_init(&ust, u->n) != 0 || live_fill_univ_state(u, &ust) != 0) {
        fprintf(stderr, "univ_state init failed\n");
        return 1;
    }
    state_mgr_init(&smgr, u);
    memset(sk, 0x51, sizeof(sk));
    memset(wr, 0x52, sizeof(wr));
    memset(ro, 0x53, sizeof(ro));
    memset(alt, 0xA1, sizeof(alt));
    if (route0_v0_compile(sk, alt, wr, ro, ROUTE0_CU_LIMIT, &v0) != 0
        || route0_signer_init(&signer, seed) != 0) {
        fprintf(stderr, "v0 failed\n");
        return 1;
    }
    if (hot_seen_init(&seen) != 0) {
        return 1;
    }
    memset(&alts, 0, sizeof(alts));
    memset(&trigm, 0, sizeof(trigm));
    if (alt_cache_init(&alts) != 0) {
        fprintf(stderr, "alt init failed\n");
        return 1;
    }
    {
        const char *altbin = "/home/louis/captures/trigger011/alt_cache.bin";
        (void)alt_cache_load(&alts, altbin);
    }
    fprintf(stderr,
            "UNIV-GEN  gen=%u n=%u routes=%u watch=%u alts=%u FUNDED=%d exact-only\n",
            gen->gen_id, gen->n_pools, u->routes.n_route, gen->watch.n, alts.n,
            FUNDED);
    if (syncrec_open(&sync, sync_path, (uint64_t)clk.hz) != 0
        || syncrec_state_init(&sync, u, now_ns()) != 0) {
        fprintf(stderr, "sync open failed\n");
        return 1;
    }
    if (flow_idx != NULL) {
        snprintf(idx_paths[n_idx], 1024, "%s", flow_idx);
        n_idx++;
    }
    if (flow_dir != NULL) {
        n_idx += list_idx(flow_dir, &idx_paths[n_idx], 32 - n_idx);
    }
    {
        int i;
        for (i = 0; i < n_idx; i++) {
            flow_load(flow, FLOW_CAP, idx_paths[i], &flow_n);
        }
    }
    if (pend_path != NULL) {
        pq.pend = fopen(pend_path, "a");
    }
    pq.recon_path = recon_path;
    (void)recon_open(&pq, recon_path);
    if (audit_path != NULL) {
        auditf = fopen(audit_path, "a");
    }
    {
        uint32_t r0 = 0, fam5 = 0, hop3 = 0, sendable = 0, ri;
        for (ri = 0; ri < u->routes.n_route; ri++) {
            if (u->routes.route[ri].family == ROUTE_FAM_0_DLMM_PUMP) {
                r0++;
            } else if (u->routes.route[ri].family == ROUTE_FAM_5_DLMM_PUMP_TYPED) {
                fam5++;
            }
            if (u->routes.route[ri].n_hop >= 3) {
                hop3++;
            }
        }
        for (ri = 0; ri < u->n; ri++) {
            if (state_sendable(&smgr, u, ri) && smgr.auth_ns[ri] != 0) {
                sendable++;
            }
        }
        fprintf(stderr,
                "PAPER-ORBIT  pools=%u route0=%u typed5=%u hop3=%u sendable=%u slot=%" PRIu64
                " ver=%" PRIu64 " flowra_idx=%" PRIu64
                "  NO SEND  follow %s  STATE-006\n",
                u->n, r0, fam5, hop3, sendable, u->slot, u->state_version, flow_n, dir);
        fflush(stderr);
    }
    fprintf(stderr,
            "  note: OrbitFlare ~7k shred/s at ingress vs ~50k/s prior premium trial — log, do not assume equivalence\n");
    memset(&rcx, 0, sizeof(rcx));
    rcx.u = u;
    rcx.st = &ust;
    rcx.smgr = &smgr;
    rcx.sync = &sync;
    rcx.pq = &pq;
    rcx.acc = &acc;
    rcx.seen = &seen;
    rcx.v0 = &v0;
    rcx.signer = &signer;
    rcx.clk = &clk;
    rcx.auditf = auditf;
    rcx.alts = &alts;
    rcx.watch = &gen->watch;
    rcx.trigm = &trigm;

    t0 = now_ns();
    last_tel = t0;
    last_flow = t0;
    cap_path[0] = 0;

    while (now_ns() - t0 < seconds * 1000000000ull) {
        feed_slot_t slot;
        int rc;

        if (cf == NULL) {
            if (newest_cap(dir, cap_path, sizeof(cap_path)) != 0) {
                usleep(50000);
                continue;
            }
            cf = fopen(cap_path, "rb");
            if (cf == NULL) {
                usleep(50000);
                continue;
            }
            if (feedcap_read_header(cf, &hdr) != 0) {
                fclose(cf);
                cf = NULL;
                usleep(50000);
                continue;
            }
            if (start_at_eof) {
                if (fseek(cf, 0, SEEK_END) != 0) {
                    fclose(cf);
                    cf = NULL;
                    continue;
                }
                fprintf(stderr, "follow %s from EOF\n", cap_path);
                start_at_eof = 0;
            } else {
                fprintf(stderr, "follow %s from header (rotate)\n", cap_path);
            }
        }
        rc = follow_slot(cf, &slot);
        if (rc == 2) {
            continue;
        }
        if (rc == 1) {
            char newer[1024];
            usleep(2000);
            if (newest_cap(dir, newer, sizeof(newer)) == 0
                && strcmp(newer, cap_path) != 0) {
                fclose(cf);
                cf = NULL;
                snprintf(cap_path, sizeof(cap_path), "%s", newer);
            }
            if (now_ns() - last_flow > 2000000000ull) {
                int i;
                if (flow_dir != NULL) {
                    n_idx = list_idx(flow_dir, idx_paths, 32);
                }
                for (i = 0; i < n_idx; i++) {
                    flow_load(flow, FLOW_CAP, idx_paths[i], &flow_n);
                }
                last_flow = now_ns();
            }
            recon_drain(&pq, u, &smgr);
            {
                static uint64_t last_alt = 0;
                struct stat st;
                if (now_ns() - last_alt > 1000000000ull) {
                    const char *altbin = "/home/louis/captures/trigger011/alt_cache.bin";
                    if (stat(altbin, &st) == 0) {
                        (void)alt_cache_load(&alts, altbin);
                    }
                    last_alt = now_ns();
                }
            }
            if (now_ns() - last_tel >= 1000000000ull) {
                uint64_t up = (now_ns() - t0) / 1000000000ull;
                uint64_t dlt = acc.shred - last_shred;
                if (acc.n_rate < RATE_CAP) {
                    acc.rate[acc.n_rate++] = (uint32_t)dlt;
                }
                telemetry(&acc, u, &smgr, up, dlt);
                last_shred = acc.shred;
                last_tel = now_ns();
            }
            continue;
        }
        if (rc != 0) {
            fclose(cf);
            cf = NULL;
            continue;
        }
        {
            shred_view_t v;
            uint16_t plen;
            swapix_t nix;
            hot_trigger_t trig;
            uint64_t c0;
            int is_data;
            fslot_t *fs = NULL;

            if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
                acc.bad++;
                continue;
            }
            acc.shred++;
            is_data = shred_is_data(v.type);
            if (is_data) {
                acc.data++;
            } else if (shred_is_code(v.type)) {
                acc.coding++;
            }
            if (acc.first_slot == 0) {
                acc.first_slot = v.slot;
            }
            if (acc.last_slot != 0 && v.slot > acc.last_slot + 1ull) {
                acc.slot_gap += v.slot - acc.last_slot - 1ull;
            }
            if (v.slot > acc.last_slot) {
                acc.last_slot = v.slot;
            }
            fec_note(fec, FEC_CAP, &v, is_data);
            plen = (uint16_t)(slot.len - (uint16_t)(v.payload - slot.data));
            if (is_data && plen > 0) {
                fs = fslot_get(v.slot, auditf, &acc);
                (void)fslot_store(fs, v.index, v.payload, plen, slot.rx_ns);
                hold_retry_slot(&rcx, fs, &v, slot.rx_ns);
            }
            if (classify_n(v.payload, plen, 6) == REL_N_NONE) {
                trigger_hit_t th;
                /* Slot-only bank: expand cached ALT addresses via live metadata.
                 * alt_cache_check still reports ALT_BANK_UNPROVEN (no SlotHashes). */
                alt_bank_t alt_bank = {.slot = v.slot};
                if (trigger_scan_at(v.payload, plen, rcx.alts, rcx.watch,
                                    &alt_bank, &th, rcx.trigm) != 0) {
                    continue;
                }
                acc.trig_generic++;
                if (th.klass == TRIG_EXACT && th.exact.have_sig
                    && swapix_to_trigger(&th.exact, &trig) == 0) {
                    acc.trig_exact++;
                    nix = th.exact;
                    c0 = rdtscp();
                } else {
                    if (th.klass == TRIG_RELEVANT_UNKNOWN || th.klass == TRIG_ALT_UNCERTAIN) {
                        acc.trig_unknown++;
                    }
                    if (th.klass == TRIG_ALT_MISS) {
                        acc.trig_alt_miss++;
                    }
                    {
                        static FILE *t011;
                        char sig[129], outer[65], altpk[65];
                        if (t011 == NULL) {
                            t011 = fopen("/home/louis/captures/trigger011/events.jsonl", "a");
                        }
                        hex_encode(th.have_sig ? th.sig : th.exact.sig, 64, sig);
                        hex_encode(th.outer, 32, outer);
                        hex_encode(th.alt_miss_pk, 32, altpk);
                        if (t011 != NULL) {
                            fprintf(t011,
                                    "{\"kind\":\"trigger011\",\"klass\":%u"
                                    ",\"relevant\":%u,\"n_watch\":%u"
                                    ",\"alt_miss\":%u,\"nlut\":%u"
                                    ",\"alt_status\":%u,\"shred_slot\":%" PRIu64
                                    ",\"n_loaded\":%u,\"pool0\":%u"
                                    ",\"outer_hex\":\"%s\",\"alt_hex\":\"%s\""
                                    ",\"sig_hex\":\"%s\"}\n",
                                    (unsigned)th.klass, (unsigned)th.relevant,
                                    (unsigned)th.n_watch, (unsigned)th.alt_miss,
                                    (unsigned)th.nlut, (unsigned)th.alt_status, v.slot, (unsigned)th.n_loaded,
                                    th.n_watch ? th.hit_pool[0] : 0u,
                                    outer, altpk, sig);
                            fflush(t011);
                        }
                        if (th.klass == TRIG_ALT_MISS || th.klass == TRIG_ALT_UNCERTAIN) {
                            static FILE *missf;
                            if (missf == NULL) {
                                missf = fopen("/home/louis/captures/trigger011/alt_miss.jsonl", "a");
                            }
                            if (missf != NULL) {
                                fprintf(missf, "{\"alt_hex\":\"%s\"}\n", altpk);
                                fflush(missf);
                            }
                        }
                    }
                    continue;
                }
            } else {
            acc.relevant++;
            c0 = rdtscp();
            if (swapix_from_payload(v.payload, plen, &nix) != 0
                || swapix_to_trigger(&nix, &trig) != 0) {
                acc.decode_fail++;
                continue;
            }
            }
            acc.decoded++;
            if (hold_find(nix.sig) != NULL) {
                acc.dup++;
                continue;
            }
            if (hot_seen_first(&seen, nix.sig) != 1) {
                acc.dup++;
                continue;
            }
            {
                uint64_t fm = 0;
                if (flow_find(flow, FLOW_CAP, nix.sig, &fm)) {
                    int64_t lead = (int64_t)slot.rx_ns - (int64_t)fm;
                    acc.both++;
                    acc.lead_n++;
                    acc.lead_sum += lead;
                    if (lead < acc.lead_min) {
                        acc.lead_min = lead;
                    }
                    if (lead > acc.lead_max) {
                        acc.lead_max = lead;
                    }
                }
            }
            if (fs == NULL) {
                fs = fslot_get(v.slot, auditf, &acc);
                (void)fslot_store(fs, v.index, v.payload, plen, slot.rx_ns);
            }
            (void)close_candidate(&rcx, fs, &nix, &trig, &v, slot.rx_ns,
                                  v.index, slot.rx_ns, c0, 0);
        }
    }
    if (cf != NULL) {
        fclose(cf);
    }
    fec_tally(fec, FEC_CAP, &acc);
    {
        FILE *sf;
        uint32_t p50a = pct_u32(acc.act, acc.n_act, 0.50);
        uint32_t p99a = pct_u32(acc.act, acc.n_act, 0.99);
        uint32_t p50s = pct_u32(acc.sgn, acc.n_sgn, 0.50);
        uint32_t p99s = pct_u32(acc.sgn, acc.n_sgn, 0.99);
        uint32_t p50r = pct_u32(acc.rate, acc.n_rate, 0.50);
        uint32_t p90r = pct_u32(acc.rate, acc.n_rate, 0.90);
        uint32_t p10f = pct_u32(acc.fdelay, acc.n_fdelay, 0.10);
        uint32_t p50f = pct_u32(acc.fdelay, acc.n_fdelay, 0.50);
        uint32_t p90f = pct_u32(acc.fdelay, acc.n_fdelay, 0.90);
        uint32_t p99f = pct_u32(acc.fdelay, acc.n_fdelay, 0.99);
        int64_t lead_avg = acc.lead_n ? acc.lead_sum / (int64_t)acc.lead_n : 0;
        printf("PAPER-ORBIT DONE\n");
        printf("shreds              %" PRIu64 "\n", acc.shred);
        printf("data/coding         %" PRIu64 " / %" PRIu64 "\n", acc.data, acc.coding);
        printf("decoded_n           %" PRIu64 "\n", acc.decoded);
        printf("decode_fail         %" PRIu64 "\n", acc.decode_fail);
        printf("dup                 %" PRIu64 "\n", acc.dup);
        printf("known_pool          %" PRIu64 "\n", acc.known);
        printf("state_sufficient    %" PRIu64 "\n", acc.state_ok);
        printf("missing_state       %" PRIu64 "\n", acc.missing_state);
        printf("hot_decide          %" PRIu64 "\n", acc.decide);
        printf("opp                 %" PRIu64 "\n", acc.opp);
        printf("signed_ready        %" PRIu64 "\n", acc.signed_ready);
        printf("stale               %" PRIu64 "\n", acc.stale);
        printf("state_version       %" PRIu64 "\n", u->state_version);
        printf("slot_range          %" PRIu64 " .. %" PRIu64 "  gaps=%" PRIu64 "\n",
               acc.first_slot, acc.last_slot, acc.slot_gap);
        printf("fec_incomplete      %" PRIu64 "\n", acc.fec_incomplete);
        printf("rx_per_s            p50=%u p90=%u  (prior trial ~50k)\n", p50r, p90r);
        printf("frame               inv=%" PRIu64 " hold=%" PRIu64
               " framed=%" PRIu64 " exp=%" PRIu64 "\n",
               acc.frame_invalid, acc.frame_incomplete, acc.frame_framed,
               acc.frame_expired);
        printf("frame_delay_ns      n=%u p10=%u p50=%u p90=%u p99=%u\n",
               acc.n_fdelay, p10f, p50f, p90f, p99f);
        printf("act->decide         n=%u p50=%u p99=%u ns\n", acc.n_act, p50a, p99a);
        printf("act->signed         n=%u p50=%u p99=%u ns\n", acc.n_sgn, p50s, p99s);
        printf("flowra_both         %" PRIu64 "  lead_ns avg=%" PRId64 " min=%" PRId64 " max=%" PRId64 "\n",
               acc.both, lead_avg, acc.lead_n ? acc.lead_min : 0, acc.lead_max);
        printf("NO SEND\n");
        if (stats_path != NULL) {
            sf = fopen(stats_path, "w");
            if (sf != NULL) {
                fprintf(sf,
                        "{\n"
                        "  \"shreds\": %" PRIu64 ",\n"
                        "  \"data\": %" PRIu64 ", \"coding\": %" PRIu64 ",\n"
                        "  \"relevant\": %" PRIu64 ",\n"
                        "  \"decoded_n\": %" PRIu64 ",\n"
                        "  \"decode_fail\": %" PRIu64 ",\n"
                        "  \"dup\": %" PRIu64 ",\n"
                        "  \"known_pool\": %" PRIu64 ",\n"
                        "  \"state_sufficient\": %" PRIu64 ",\n"
                        "  \"missing_state\": %" PRIu64 ",\n"
                        "  \"hot_decide\": %" PRIu64 ",\n"
                        "  \"opp\": %" PRIu64 ",\n"
                        "  \"signed_ready\": %" PRIu64 ",\n"
                        "  \"stale\": %" PRIu64 ",\n"
                        "  \"state_version\": %" PRIu64 ",\n"
                        "  \"univ_slot\": %" PRIu64 ",\n"
                        "  \"first_slot\": %" PRIu64 ", \"last_slot\": %" PRIu64 ",\n"
                        "  \"slot_gap\": %" PRIu64 ",\n"
                        "  \"fec_incomplete\": %" PRIu64 ",\n"
                        "  \"rx_p50\": %u, \"rx_p90\": %u,\n"
                        "  \"act_p50_ns\": %u, \"act_p99_ns\": %u,\n"
                        "  \"sgn_p50_ns\": %u, \"sgn_p99_ns\": %u,\n"
                        "  \"flowra_both\": %" PRIu64 ",\n"
                        "  \"lead_ns_avg\": %" PRId64 ",\n"
                        "  \"lead_ns_min\": %" PRId64 ",\n"
                        "  \"lead_ns_max\": %" PRId64 ",\n"
                        "  \"send\": false\n"
                        "}\n",
                        acc.shred, acc.data, acc.coding, acc.relevant,
                        acc.decoded, acc.decode_fail, acc.dup, acc.known,
                        acc.state_ok, acc.missing_state, acc.decide, acc.opp,
                        acc.signed_ready, acc.stale, u->state_version, u->slot,
                        acc.first_slot, acc.last_slot, acc.slot_gap,
                        acc.fec_incomplete,
                        p50r, p90r, p50a, p99a, p50s, p99s,
                        acc.both, lead_avg, acc.lead_n ? acc.lead_min : 0,
                        acc.lead_max);
                fclose(sf);
            }
        }
    }
    syncrec_close(&sync);
    hot_seen_free(&seen);
    alt_cache_free(&alts);
    state_mgr_free(&smgr);
    authpub_close();
    (void)universe_gen_publish(NULL);
    universe_gen_free(gen);
    free(gen);
    free(g_mut_slot);
    free(g_s007_updating);
    free(g_s007_gen);
    free(g_s007_rx_ns);
    {
        uint32_t si;
        for (si = 0; si < F_SLOTS; si++) {
            free(g_fslot[si].arena);
            g_fslot[si].arena = NULL;
        }
    }
    free(acc.act);
    free(acc.sgn);
    free(acc.fdelay);
    free(fec);
    free(flow);
    return 0;
}
