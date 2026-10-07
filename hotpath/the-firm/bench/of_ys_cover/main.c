#include "net/capio.h"
#include "shred/assemble.h"
#include "shred/fec.h"
#include "shred/shred.h"
#include "shred/sig.h"
#include "wire/classify.h"
#include "wire/lut_file.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * OF-YS-COVER-001. Overlap accounting. No FAST_CUSTOM. No AUTH import.
 * YS jsonl is the pAMM-account Geyser filter, not a full slot ledger.
 */

#define YS_MAX     400000u
#define OF_TX_MAX  200000u
#define SLOT_CAP   131072u
#define PROBE_MAX  256u
#define OFH_CAP    262144u

#define LOSS_NO_OF_SLOT   0u
#define LOSS_ABSENT_RAW   1u
#define LOSS_RAW_NO_RECON 2u
#define LOSS_RECON_BADSIG 3u
#define LOSS_RECON_OK     4u

#define ST_FEED_MISSING 0u
#define ST_WRONG_BRANCH 1u
#define ST_PREFIX_GAP   2u
#define ST_ENTRY_PARSE  3u
#define ST_TX_PARSE     4u
#define ST_CLASSIFIER   5u
#define ST_OK           6u

#define DIAG_IX 32768u
#define DIAG_NW (DIAG_IX / 32u)

typedef struct {
    uint8_t  used;
    uint8_t  pamm;
    uint8_t  version;
    uint8_t  has_index;
    uint32_t index;
    uint64_t slot;
    uint8_t  sig[64];
    char     hex[129];
} ys_row_t;

typedef struct {
    uint8_t  used;
    uint8_t  verified;
    uint8_t  encoding;
    uint8_t  klass;
    uint8_t  lut_miss;
    uint8_t  pamm;
    uint8_t  bond;
    uint32_t entry;
    uint32_t tx_ord;
    uint32_t tlen;
    uint64_t slot;
    uint8_t  sig[64];
} of_tx_t;

typedef struct {
    uint8_t  used;
    uint64_t slot;
    uint32_t of_pkts;
    uint32_t of_data;
    uint32_t of_code;
    uint32_t of_max_ix;
    uint32_t ys_n;
    uint32_t of_n;
    uint32_t inter;
    uint32_t ys_only;
    uint32_t of_only;
} slot_acc_t;

typedef struct {
    uint8_t  used;
    uint8_t  raw;
    uint8_t  recon;
    uint8_t  verok;
    uint8_t  raw_data;
    uint8_t  raw_code;
    uint8_t  klass;
    uint8_t  encoding;
    uint8_t  lut_miss;
    uint32_t raw_index;
    uint32_t raw_fec;
    uint32_t ys_index;
    uint32_t of_tx_ord;
    uint32_t first_hole;
    uint8_t  ys_version;
    uint8_t  have0;
    uint8_t  n_root;
    uint8_t  ix_conflict;
    uint8_t  below_wm;
    uint8_t  idx_match;
    uint64_t slot;
    uint8_t  sig[64];
    char     hex[129];
} probe_t;

typedef struct {
    uint32_t index;
    uint8_t  used;
    uint8_t  root[32];
} diag_ixroot_t;

typedef struct {
    uint8_t  used;
    uint8_t  n_root;
    uint8_t  have0;
    uint32_t n_data;
    uint32_t max_ix;
    uint32_t first_hole;
    uint32_t n_ix_conflict;
    uint64_t slot;
    uint8_t  root[8][32];
    uint32_t bits[DIAG_NW];
    diag_ixroot_t ixr[512];
} diag_slot_t;

static ys_row_t *g_ys;
static of_tx_t *g_of;
static slot_acc_t g_slot[SLOT_CAP];
static int32_t g_ofh[OFH_CAP];
static int32_t g_ysh[OFH_CAP];
static uint32_t nys, nof;
static diag_slot_t g_diag[PROBE_MAX];
static uint32_t ndiag;

static void ysh_put(uint32_t idx);
static void ysh_clear(void);

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
unhex64(const char *s, uint8_t *o)
{
    uint32_t i;

    for (i = 0; i < 64u; i++) {
        int a = hexval(s[2u * i]);
        int b = hexval(s[2u * i + 1u]);

        if (a < 0 || b < 0) {
            return -1;
        }
        o[i] = (uint8_t)((a << 4) | b);
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

static const uint8_t *
find64(const uint8_t *hay, uint32_t n, const uint8_t *need)
{
    uint32_t i;

    if (n < 64u) {
        return NULL;
    }
    for (i = 0; i + 64u <= n; i++) {
        if (hay[i] == need[0] && memcmp(hay + i, need, 64) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static uint32_t
shash64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    return (uint32_t)x;
}

static uint32_t
shash_sig(const uint8_t *s)
{
    uint64_t x;

    memcpy(&x, s, 8);
    return shash64(x);
}

static slot_acc_t *
slot_get(uint64_t slot, int create)
{
    uint32_t i, h = shash64(slot) & (SLOT_CAP - 1u);

    for (i = 0; i < SLOT_CAP; i++) {
        uint32_t k = (h + i) & (SLOT_CAP - 1u);
        slot_acc_t *r = &g_slot[k];

        if (!r->used) {
            if (!create) {
                return NULL;
            }
            r->used = 1;
            r->slot = slot;
            return r;
        }
        if (r->slot == slot) {
            return r;
        }
    }
    return NULL;
}

static diag_slot_t *
diag_get(uint64_t slot, int create)
{
    uint32_t i;

    for (i = 0; i < ndiag; i++) {
        if (g_diag[i].used && g_diag[i].slot == slot) {
            return &g_diag[i];
        }
    }
    if (!create || ndiag >= PROBE_MAX) {
        return NULL;
    }
    g_diag[ndiag].used = 1;
    g_diag[ndiag].slot = slot;
    g_diag[ndiag].first_hole = UINT32_MAX;
    return &g_diag[ndiag++];
}

static void
diag_note_index(diag_slot_t *d, uint32_t index)
{
    if (d == NULL || index >= DIAG_IX) {
        return;
    }
    d->bits[index / 32u] |= (1u << (index % 32u));
    d->n_data++;
    if (index == 0) {
        d->have0 = 1;
    }
    if (index > d->max_ix) {
        d->max_ix = index;
    }
}

static void
diag_note_root(diag_slot_t *d, uint32_t index, const uint8_t root[32])
{
    uint32_t i;

    if (d == NULL || root == NULL) {
        return;
    }
    for (i = 0; i < d->n_root; i++) {
        if (memcmp(d->root[i], root, 32) == 0) {
            break;
        }
    }
    if (i == d->n_root && d->n_root < 8u) {
        memcpy(d->root[d->n_root], root, 32);
        d->n_root++;
    }
    for (i = 0; i < 512u; i++) {
        if (!d->ixr[i].used) {
            d->ixr[i].used = 1;
            d->ixr[i].index = index;
            memcpy(d->ixr[i].root, root, 32);
            return;
        }
        if (d->ixr[i].index == index) {
            if (memcmp(d->ixr[i].root, root, 32) != 0) {
                d->n_ix_conflict++;
            }
            return;
        }
    }
}

static void
diag_finish(void)
{
    uint32_t i, j;

    for (i = 0; i < ndiag; i++) {
        diag_slot_t *d = &g_diag[i];
        uint32_t lim = d->max_ix + 1u;

        if (lim > DIAG_IX) {
            lim = DIAG_IX;
        }
        d->first_hole = lim;
        for (j = 0; j < lim; j++) {
            if ((d->bits[j / 32u] & (1u << (j % 32u))) == 0) {
                d->first_hole = j;
                break;
            }
        }
    }
}

static int
pamm_ixs(const uint8_t *tx, uint32_t len)
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

static int
bond_ixs(const uint8_t *tx, uint32_t len)
{
    wire_ix_view_t ix[WIRE_IX_VIEW_MAX];
    uint32_t n = 0, i;

    if (wire_tx_ixs(tx, len, ix, WIRE_IX_VIEW_MAX, &n) != 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (ix[i].fam == WIRE_FAM_PUMP_BOND) {
            return 1;
        }
    }
    return 0;
}

static int
load_ys(const char *path, uint64_t slot_lo, uint64_t slot_hi)
{
    FILE *f;
    char line[2048];

    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    nys = 0;
    ysh_clear();
    while (fgets(line, sizeof(line), f) != NULL && nys < YS_MAX) {
        const char *sig;
        ys_row_t *r;
        uint64_t slot;

        if (strstr(line, "\"ys_tx\"") == NULL) {
            continue;
        }
        if (strstr(line, "\"pamm\":true") == NULL
            && strstr(line, "\"pamm\": true") == NULL) {
            continue;
        }
        slot = json_u64(line, "\"slot\"");
        if (slot < slot_lo || slot > slot_hi) {
            continue;
        }
        sig = strstr(line, "\"sig_hex\"");
        if (sig == NULL) {
            continue;
        }
        sig = strchr(sig, ':');
        if (sig == NULL) {
            continue;
        }
        sig = strchr(sig, '"');
        if (sig == NULL) {
            continue;
        }
        sig++;
        r = &g_ys[nys];
        memset(r, 0, sizeof(*r));
        if (unhex64(sig, r->sig) != 0) {
            continue;
        }
        hexsig(r->hex, r->sig);
        r->used = 1;
        r->pamm = 1;
        r->slot = slot;
        r->version = (uint8_t)json_u64(line, "\"version\"");
        if (strstr(line, "\"index\"") != NULL) {
            r->has_index = 1;
            r->index = (uint32_t)json_u64(line, "\"index\"");
        }
        ysh_put(nys);
        nys++;
    }
    fclose(f);
    return 0;
}

static void
ofh_clear(void)
{
    uint32_t i;

    for (i = 0; i < OFH_CAP; i++) {
        g_ofh[i] = -1;
    }
}

static void
ofh_put(uint32_t idx)
{
    uint32_t i, h = shash_sig(g_of[idx].sig) & (OFH_CAP - 1u);

    for (i = 0; i < OFH_CAP; i++) {
        uint32_t k = (h + i) & (OFH_CAP - 1u);

        if (g_ofh[k] < 0) {
            g_ofh[k] = (int32_t)idx;
            return;
        }
    }
}

static void
ysh_clear(void)
{
    uint32_t i;

    for (i = 0; i < OFH_CAP; i++) {
        g_ysh[i] = -1;
    }
}

static void
ysh_put(uint32_t idx)
{
    uint32_t i, h = shash_sig(g_ys[idx].sig) & (OFH_CAP - 1u);

    for (i = 0; i < OFH_CAP; i++) {
        uint32_t k = (h + i) & (OFH_CAP - 1u);

        if (g_ysh[k] < 0) {
            g_ysh[k] = (int32_t)idx;
            return;
        }
    }
}

static const ys_row_t *
ysh_find(const uint8_t *sig)
{
    uint32_t i, h = shash_sig(sig) & (OFH_CAP - 1u);

    for (i = 0; i < OFH_CAP; i++) {
        uint32_t k = (h + i) & (OFH_CAP - 1u);
        int32_t ix = g_ysh[k];

        if (ix < 0) {
            return NULL;
        }
        if (memcmp(g_ys[ix].sig, sig, 64) == 0) {
            return &g_ys[ix];
        }
    }
    return NULL;
}

static const of_tx_t *
ofh_find(const uint8_t *sig)
{
    uint32_t i, h = shash_sig(sig) & (OFH_CAP - 1u);

    for (i = 0; i < OFH_CAP; i++) {
        uint32_t k = (h + i) & (OFH_CAP - 1u);
        int32_t ix = g_ofh[k];

        if (ix < 0) {
            return NULL;
        }
        if (memcmp(g_of[ix].sig, sig, 64) == 0) {
            return &g_of[ix];
        }
    }
    return NULL;
}

static int
find_shred(const uint8_t *pkt, uint16_t len, shred_view_t *v,
           const uint8_t **sp, uint16_t *slen)
{
    static const uint16_t offs[] = { 0, 8, 28, 42 };
    uint32_t i;

    for (i = 0; i < 4u; i++) {
        uint16_t o = offs[i];
        uint16_t rem;

        if (len <= o) {
            continue;
        }
        rem = (uint16_t)(len - o);
        if (rem > SHRED_MAX_SZ) {
            rem = (uint16_t)SHRED_MAX_SZ;
        }
        if (shred_parse(pkt + o, rem, v) == 0) {
            *sp = pkt + o;
            *slen = rem;
            return 0;
        }
    }
    return -1;
}

static const char *
loss_name(uint8_t s)
{
    switch (s) {
    case LOSS_NO_OF_SLOT:
        return "NO_OF_SLOT";
    case LOSS_ABSENT_RAW:
        return "ABSENT_RAW";
    case LOSS_RAW_NO_RECON:
        return "RAW_NO_RECON";
    case LOSS_RECON_BADSIG:
        return "RECON_BADSIG";
    case LOSS_RECON_OK:
        return "RECON_OK";
    default:
        return "OTHER";
    }
}

static const char *
stage_name(uint8_t s)
{
    switch (s) {
    case ST_FEED_MISSING:
        return "FEED_MISSING";
    case ST_WRONG_BRANCH:
        return "WRONG_BRANCH";
    case ST_PREFIX_GAP:
        return "PREFIX_GAP";
    case ST_ENTRY_PARSE:
        return "ENTRY_PARSE";
    case ST_TX_PARSE:
        return "TX_PARSE";
    case ST_CLASSIFIER:
        return "CLASSIFIER";
    case ST_OK:
        return "OK";
    default:
        return "OTHER";
    }
}

static uint8_t
probe_stage(const probe_t *p, int of_slot, uint32_t slot_of_n)
{
    if (!of_slot || !p->raw) {
        return ST_FEED_MISSING;
    }
    if (p->ix_conflict) {
        return ST_WRONG_BRANCH;
    }
    if (!p->have0 || !p->below_wm) {
        return ST_PREFIX_GAP;
    }
    if (p->recon && p->verok) {
        if (!p->idx_match) {
            return ST_TX_PARSE;
        }
        if (p->lut_miss || p->klass == WIRE_CL_UNRESOLVED_ALT) {
            return ST_CLASSIFIER;
        }
        return ST_OK;
    }
    if (p->recon && !p->verok) {
        return ST_TX_PARSE;
    }
    if (slot_of_n == 0) {
        return ST_ENTRY_PARSE;
    }
    return ST_TX_PARSE;
}

static uint8_t
probe_loss(const probe_t *p, int of_slot)
{
    if (!of_slot) {
        return LOSS_NO_OF_SLOT;
    }
    if (!p->raw) {
        return LOSS_ABSENT_RAW;
    }
    if (!p->recon) {
        return LOSS_RAW_NO_RECON;
    }
    if (!p->verok) {
        return LOSS_RECON_BADSIG;
    }
    return LOSS_RECON_OK;
}

static int
self_test(void)
{
    uint8_t sig[64], buf[200];
    uint32_t i;

    for (i = 0; i < 64u; i++) {
        sig[i] = (uint8_t)(i + 3u);
    }
    memset(buf, 0x11, sizeof(buf));
    memcpy(buf + 40, sig, 64);
    if (find64(buf, sizeof(buf), sig) != buf + 40) {
        fprintf(stderr, "FAIL find64\n");
        return 1;
    }
    if (slot_get(42, 1) == NULL || slot_get(42, 0) == NULL) {
        fprintf(stderr, "FAIL slot\n");
        return 1;
    }
    printf("OF-YS-COVER-001 self-test ok\n");
    return 0;
}

int
main(int argc, char **argv)
{
    capio_t cap;
    shred_assem_t *as = NULL;
    wire_lut_tab_t luts;
    probe_t pr[PROBE_MAX];
    const char *cpath = NULL, *yspath = NULL, *lutpath = NULL;
    const char *pout = NULL, *sout = NULL;
    uint32_t probe_n = 100, i, pkts = 0, parsed = 0;
    uint32_t npr = 0, n_overlap = 0, n_ys_ol = 0, n_inter = 0;
    uint64_t of_slot_lo = UINT64_MAX, of_slot_hi = 0;
    uint64_t ys_slot_lo = UINT64_MAX, ys_slot_hi = 0;
    uint64_t yo_leg = 0, yo_v0 = 0, yo_v1 = 0, yo_pamm = 0;
    uint64_t yo_idx0 = 0, yo_idxm = 0, yo_idxh = 0;
    uint64_t ofo_pamm = 0, ofo_bond = 0, ofo_other = 0, ofo_alt = 0;
    uint64_t loss[5] = { 0 };
    uint64_t stagec[7] = { 0 };
    uint32_t raw_have0 = 0, raw_multi_root = 0, raw_below = 0;
    uint32_t raw_hit = 0, recon_hit = 0;
    int self = 0;
    FILE *pf = NULL, *sf = NULL;
    const shred_stream_stats_t *sst;
    const shred_fec_stats_t *fst;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--cap") == 0 && i + 1 < (uint32_t)argc) {
            cpath = argv[++i];
        } else if (strcmp(argv[i], "--ys") == 0 && i + 1 < (uint32_t)argc) {
            yspath = argv[++i];
        } else if (strcmp(argv[i], "--luts") == 0 && i + 1 < (uint32_t)argc) {
            lutpath = argv[++i];
        } else if (strcmp(argv[i], "--probe") == 0 && i + 1 < (uint32_t)argc) {
            probe_n = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--probes-out") == 0
                   && i + 1 < (uint32_t)argc) {
            pout = argv[++i];
        } else if (strcmp(argv[i], "--slots-out") == 0
                   && i + 1 < (uint32_t)argc) {
            sout = argv[++i];
        } else if (strcmp(argv[i], "--self-test") == 0) {
            self = 1;
        }
    }
    if (probe_n > PROBE_MAX) {
        probe_n = PROBE_MAX;
    }
    if (self) {
        return self_test();
    }
    if (cpath == NULL || yspath == NULL) {
        fprintf(stderr,
                "usage: of_ys_cover --cap FILE --ys FILE [--probe N] "
                "[--luts FILE] [--probes-out FILE] [--slots-out FILE]\n");
        return 2;
    }
    g_ys = calloc(YS_MAX, sizeof(*g_ys));
    g_of = calloc(OF_TX_MAX, sizeof(*g_of));
    if (g_ys == NULL || g_of == NULL) {
        return 1;
    }
    memset(pr, 0, sizeof(pr));
    ofh_clear();
    ysh_clear();
    wire_lut_tab_clear(&luts);
    if (lutpath != NULL) {
        (void)wire_lut_load_jsonl(&luts, lutpath);
    }
    if (capio_open(&cap, cpath) != 0) {
        fprintf(stderr, "cap open failed\n");
        return 1;
    }
    while (1) {
        net_slot_t sl;
        shred_view_t v;
        const uint8_t *sp;
        uint16_t slen;
        slot_acc_t *sa;
        int rc = capio_read(&cap, &sl);

        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            continue;
        }
        pkts++;
        if (find_shred(sl.data, sl.len, &v, &sp, &slen) != 0) {
            continue;
        }
        parsed++;
        if (v.slot < of_slot_lo) {
            of_slot_lo = v.slot;
        }
        if (v.slot > of_slot_hi) {
            of_slot_hi = v.slot;
        }
        sa = slot_get(v.slot, 1);
        if (sa != NULL) {
            sa->of_pkts++;
            if (shred_is_data(v.type)) {
                sa->of_data++;
            }
            if (shred_is_code(v.type)) {
                sa->of_code++;
            }
            if (v.index > sa->of_max_ix) {
                sa->of_max_ix = v.index;
            }
        }
    }
    capio_close(&cap);
    if (of_slot_lo == UINT64_MAX) {
        fprintf(stderr, "no OF shred slots\n");
        return 1;
    }
    if (load_ys(yspath, of_slot_lo, of_slot_hi) != 0) {
        fprintf(stderr, "ys load failed\n");
        return 1;
    }
    for (i = 0; i < nys; i++) {
        if (g_ys[i].slot < ys_slot_lo) {
            ys_slot_lo = g_ys[i].slot;
        }
        if (g_ys[i].slot > ys_slot_hi) {
            ys_slot_hi = g_ys[i].slot;
        }
    }

    for (i = 0; i < nys && npr < probe_n; i++) {
        slot_acc_t *sa = slot_get(g_ys[i].slot, 0);

        if (sa == NULL || sa->of_pkts == 0) {
            continue;
        }
        pr[npr].used = 1;
        pr[npr].slot = g_ys[i].slot;
        pr[npr].ys_index = g_ys[i].index;
        pr[npr].ys_version = g_ys[i].version;
        memcpy(pr[npr].sig, g_ys[i].sig, 64);
        memcpy(pr[npr].hex, g_ys[i].hex, 129);
        (void)diag_get(g_ys[i].slot, 1);
        npr++;
    }

    if (shred_assem_init(&as) != 0 || capio_open(&cap, cpath) != 0) {
        fprintf(stderr, "pass2 open failed\n");
        return 1;
    }
    while (1) {
        net_slot_t sl;
        shred_batch_t batch;
        shred_view_t v;
        const uint8_t *sp;
        uint16_t slen;
        int rc, prc;
        uint32_t p;

        rc = capio_read(&cap, &sl);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            continue;
        }
        if (find_shred(sl.data, sl.len, &v, &sp, &slen) == 0) {
            diag_slot_t *ds = diag_get(v.slot, 0);
            uint8_t root[32];

            if (ds != NULL && shred_is_data(v.type)) {
                diag_note_index(ds, v.index);
                if (shred_fec_merkle_root(sp, slen, root) == 0) {
                    diag_note_root(ds, v.index, root);
                }
            }
            for (p = 0; p < npr; p++) {
                if (pr[p].raw) {
                    continue;
                }
                if (find64(sl.data, sl.len, pr[p].sig) == NULL) {
                    continue;
                }
                pr[p].raw = 1;
                pr[p].raw_index = v.index;
                pr[p].raw_fec = v.fec_set;
                pr[p].raw_data = (uint8_t)shred_is_data(v.type);
                pr[p].raw_code = (uint8_t)shred_is_code(v.type);
                raw_hit++;
            }
        }
        prc = shred_assem_push(as, sl.data, sl.len, sl.rx_tsc, &batch);
        while (prc == 1) {
            uint32_t t;

            for (t = 0; t < batch.n_tx && nof < OF_TX_MAX; t++) {
                const uint8_t *tb = batch.buf + batch.tx_off[t];
                uint32_t tl = batch.tx_len[t];
                of_tx_t *o = &g_of[nof];
                wire_class_t cl;
                slot_acc_t *sa;
                int vr;

                memset(o, 0, sizeof(*o));
                if (wire_classify(tb, tl, &luts, &cl) != 0) {
                    continue;
                }
                vr = shred_tx_sig_verify(tb, tl);
                o->used = 1;
                o->verified = (uint8_t)(vr == 1);
                o->encoding = cl.encoding;
                o->klass = cl.klass;
                o->lut_miss = cl.lut_missing;
                o->slot = batch.slot;
                o->entry = batch.tx_entry[t];
                o->tx_ord = batch.tx_ord[t];
                o->tlen = tl;
                memcpy(o->sig, cl.sig, 64);
                o->pamm = (uint8_t)pamm_ixs(tb, tl);
                o->bond = (uint8_t)bond_ixs(tb, tl);
                ofh_put(nof);
                sa = slot_get(batch.slot, 1);
                if (sa != NULL) {
                    sa->of_n++;
                }
                for (p = 0; p < npr; p++) {
                    if (memcmp(pr[p].sig, cl.sig, 64) != 0) {
                        continue;
                    }
                    pr[p].recon = 1;
                    pr[p].verok = o->verified;
                    pr[p].klass = cl.klass;
                    pr[p].encoding = cl.encoding;
                    pr[p].lut_miss = cl.lut_missing;
                    pr[p].of_tx_ord = o->tx_ord;
                    pr[p].idx_match = (uint8_t)(o->tx_ord == pr[p].ys_index);
                    recon_hit++;
                }
                nof++;
            }
            prc = shred_assem_next(as, &batch);
        }
    }
    diag_finish();
    for (i = 0; i < npr; i++) {
        diag_slot_t *ds = diag_get(pr[i].slot, 0);

        if (ds == NULL) {
            continue;
        }
        pr[i].have0 = ds->have0;
        pr[i].n_root = ds->n_root;
        pr[i].ix_conflict = (uint8_t)(ds->n_ix_conflict != 0);
        pr[i].first_hole = ds->first_hole;
        pr[i].below_wm = (uint8_t)(pr[i].raw && pr[i].raw_index < ds->first_hole);
    }

    for (i = 0; i < nys; i++) {
        slot_acc_t *sa = slot_get(g_ys[i].slot, 0);
        const of_tx_t *o;

        if (sa == NULL || sa->of_pkts == 0) {
            continue;
        }
        n_ys_ol++;
        sa->ys_n++;
        o = ofh_find(g_ys[i].sig);
        if (o != NULL) {
            n_inter++;
            sa->inter++;
        } else {
            sa->ys_only++;
            yo_pamm++;
            if (g_ys[i].version == 0) {
                yo_leg++;
            } else if (g_ys[i].version == 1) {
                yo_v1++;
            } else {
                yo_v0++;
            }
            if (sa->of_max_ix > 0 && g_ys[i].has_index) {
                uint32_t tert = (g_ys[i].index * 3u) / (sa->of_max_ix + 1u);

                if (tert == 0) {
                    yo_idx0++;
                } else if (tert == 1) {
                    yo_idxm++;
                } else {
                    yo_idxh++;
                }
            }
        }
    }
    for (i = 0; i < nof; i++) {
        slot_acc_t *sa = slot_get(g_of[i].slot, 0);

        if (sa == NULL || sa->ys_n == 0) {
            continue;
        }
        if (ysh_find(g_of[i].sig) != NULL) {
            continue;
        }
        sa->of_only++;
        if (g_of[i].pamm) {
            ofo_pamm++;
        } else if (g_of[i].bond) {
            ofo_bond++;
        } else if (g_of[i].lut_miss
                   || g_of[i].klass == WIRE_CL_UNRESOLVED_ALT) {
            ofo_alt++;
        } else {
            ofo_other++;
        }
    }
    for (i = 0; i < SLOT_CAP; i++) {
        if (g_slot[i].used && g_slot[i].of_pkts != 0 && g_slot[i].ys_n != 0) {
            n_overlap++;
        }
    }
    for (i = 0; i < npr; i++) {
        slot_acc_t *sa = slot_get(pr[i].slot, 0);
        int ofsl = sa != NULL && sa->of_pkts != 0;
        uint8_t stg = probe_loss(&pr[i], ofsl);
        uint8_t rst = probe_stage(&pr[i], ofsl, sa != NULL ? sa->of_n : 0);

        loss[stg]++;
        stagec[rst]++;
        if (pr[i].raw && pr[i].have0) {
            raw_have0++;
        }
        if (pr[i].raw && pr[i].ix_conflict) {
            raw_multi_root++;
        }
        if (pr[i].below_wm) {
            raw_below++;
        }
    }
    if (pout != NULL) {
        pf = fopen(pout, "w");
    }
    if (sout != NULL) {
        sf = fopen(sout, "w");
    }
    if (pf != NULL) {
        for (i = 0; i < npr; i++) {
            int ofsl = slot_get(pr[i].slot, 0) != NULL
                       && slot_get(pr[i].slot, 0)->of_pkts != 0;
            uint8_t stg = probe_loss(&pr[i], ofsl);

            {
                slot_acc_t *sa = slot_get(pr[i].slot, 0);
                uint8_t rst = probe_stage(&pr[i], ofsl,
                                          sa != NULL ? sa->of_n : 0);

                fprintf(pf,
                        "{\"sig\":\"%s\",\"slot\":%" PRIu64
                        ",\"ys_index\":%u,\"raw\":%u,\"recon\":%u,"
                        "\"sig_ok\":%u,\"raw_index\":%u,\"raw_fec\":%u,"
                        "\"raw_data\":%u,\"have0\":%u,\"first_hole\":%u,"
                        "\"n_root\":%u,\"ix_conflict\":%u,\"below_wm\":%u,\"of_tx_ord\":%u,"
                        "\"idx_match\":%u,\"loss\":\"%s\",\"stage\":\"%s\"}\n",
                        pr[i].hex, pr[i].slot, pr[i].ys_index, pr[i].raw,
                        pr[i].recon, pr[i].verok, pr[i].raw_index,
                        pr[i].raw_fec, pr[i].raw_data, pr[i].have0,
                        pr[i].first_hole, pr[i].n_root, pr[i].ix_conflict,
                        pr[i].below_wm,
                        pr[i].of_tx_ord, pr[i].idx_match, loss_name(stg),
                        stage_name(rst));
            }
        }
        fclose(pf);
    }
    if (sf != NULL) {
        for (i = 0; i < SLOT_CAP; i++) {
            slot_acc_t *s = &g_slot[i];

            if (!s->used || s->ys_n == 0 || s->of_pkts == 0) {
                continue;
            }
            fprintf(sf,
                    "{\"slot\":%" PRIu64 ",\"ys\":%u,\"of\":%u,\"inter\":%u,"
                    "\"ys_only\":%u,\"of_only\":%u,\"of_pkts\":%u,"
                    "\"of_data\":%u,\"of_code\":%u,\"of_max_ix\":%u}\n",
                    s->slot, s->ys_n, s->of_n, s->inter, s->ys_only,
                    s->of_only, s->of_pkts, s->of_data, s->of_code,
                    s->of_max_ix);
        }
        fclose(sf);
    }

    sst = shred_assem_stream_stats(as);
    fst = shred_assem_fec_stats(as);
    printf("OF-YS-COVER-001\n");
    printf("cap=%s  ys_pamm_rows=%u  of_txs=%u  pkts=%u parsed_shreds=%u\n",
           cpath, nys, nof, pkts, parsed);
    printf("  of_slot=[%" PRIu64 ",%" PRIu64 "]  ys_slot=[%" PRIu64
           ",%" PRIu64 "]\n",
           of_slot_lo == UINT64_MAX ? 0 : of_slot_lo, of_slot_hi,
           ys_slot_lo == UINT64_MAX ? 0 : ys_slot_lo, ys_slot_hi);
    printf("  overlap_slots=%u  ys_pamm_in_overlap=%u  intersection=%u\n",
           n_overlap, n_ys_ol, n_inter);
    printf("  YS-only=%" PRIu64 "  (ys version0=%" PRIu64 " v0ish=%" PRIu64
           " v1=%" PRIu64 ")\n",
           yo_pamm, yo_leg, yo_v0, yo_v1);
    printf("  YS-only shred-index tertile low/mid/high=%" PRIu64 "/%" PRIu64
           "/%" PRIu64 "\n",
           yo_idx0, yo_idxm, yo_idxh);
    printf("  OF-only in overlap slots: pamm=%" PRIu64 " bond=%" PRIu64
           " unresolved_alt=%" PRIu64 " other=%" PRIu64 "\n",
           ofo_pamm, ofo_bond, ofo_alt, ofo_other);
    printf("  stream txs=%" PRIu64 " sig_ok=%" PRIu64 " sig_fail=%" PRIu64
           " frozen=%" PRIu64 " gap_blocked=%" PRIu64 " leftover=%" PRIu64
           "\n",
           sst != NULL ? sst->txs : 0, sst != NULL ? sst->sig_verify_ok : 0,
           sst != NULL ? sst->sig_verify_fail : 0,
           sst != NULL ? sst->frozen : 0, sst != NULL ? sst->gap_blocked : 0,
           sst != NULL ? sst->leftover_bytes : 0);
    if (fst != NULL) {
        printf("  fec recovered=%u auth_ok=%u auth_fail=%u\n", fst->recovered,
               fst->auth_ok, fst->auth_fail);
    }
    printf("PROBE n=%u  raw_present=%u  reconstructed=%u\n", npr, raw_hit,
           recon_hit);
    printf("  NO_OF_SLOT=%" PRIu64 "  ABSENT_RAW=%" PRIu64
           "  RAW_NO_RECON=%" PRIu64 "  RECON_BADSIG=%" PRIu64
           "  RECON_OK=%" PRIu64 "\n",
           loss[LOSS_NO_OF_SLOT], loss[LOSS_ABSENT_RAW],
           loss[LOSS_RAW_NO_RECON], loss[LOSS_RECON_BADSIG],
           loss[LOSS_RECON_OK]);
    printf("RESEARCH stage (SHRED-STATE-001)\n");
    printf("  FEED_MISSING=%" PRIu64 "  WRONG_BRANCH=%" PRIu64
           "  PREFIX_GAP=%" PRIu64 "\n",
           stagec[ST_FEED_MISSING], stagec[ST_WRONG_BRANCH],
           stagec[ST_PREFIX_GAP]);
    printf("  ENTRY_PARSE=%" PRIu64 "  TX_PARSE=%" PRIu64
           "  CLASSIFIER=%" PRIu64 "  OK=%" PRIu64 "\n",
           stagec[ST_ENTRY_PARSE], stagec[ST_TX_PARSE],
           stagec[ST_CLASSIFIER], stagec[ST_OK]);
    printf("  raw probes: have_index0=%u  multi_root=%u  below_watermark=%u\n",
           raw_have0, raw_multi_root, raw_below);
    printf("  YS is pAMM-account filtered, not a full slot.\n");
    printf("  Numeric-slot stream only; conflicting roots are WRONG_BRANCH.\n");
    printf("  A decodable suffix without origin index 0 is PREFIX_GAP.\n");
    capio_close(&cap);
    shred_assem_free(as);
    free(g_ys);
    free(g_of);
    return 0;
}
