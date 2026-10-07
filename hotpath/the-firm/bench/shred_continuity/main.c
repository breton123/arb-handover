#include "net/capio.h"
#include "shred/fec.h"
#include "shred/prefix.h"
#include "shred/shred.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FROM_SLOT 450882490ull
#define TO_SLOT   450883470ull
#define PROBE_MAX 2048u
#define BAG_MAX   128u
#define SHRED_CAP 1024u
#define LINE_MAX  512u

#define CLS_A 1u /* RAW_PRESENT: in data-only prefix */
#define CLS_B 2u /* FEC_RECOVERABLE: in FEC prefix only */
#define CLS_C 3u /* FEED_MISSING */

typedef struct {
    uint8_t  used;
    uint8_t  cls;
    uint8_t  sig[64];
    uint64_t slot;
} probe_t;

typedef struct {
    uint8_t  used;
    uint8_t  is_code;
    uint32_t index;
    uint16_t len;
    uint8_t  pkt[SHRED_MAX_SZ];
} bag_shred_t;

typedef struct {
    uint8_t     used;
    uint64_t    slot;
    uint32_t    n;
    uint32_t    n_data;
    uint32_t    n_code;
    uint32_t    overflow;
    bag_shred_t sh[SHRED_CAP];
} bag_t;

static probe_t g_pr[PROBE_MAX];
static uint32_t g_npr;
static bag_t g_bag[BAG_MAX];
static uint64_t g_n_a, g_n_b, g_n_c, g_n_orphan;
static uint64_t g_slots_seen, g_slots_hole, g_slots_fec_gain;
static shred_fec_t *g_fec;
static shred_prefix_t *g_px_data, *g_px_fec;

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
    char *end = NULL;
    uint64_t v;

    if (p == NULL) {
        return 0;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return 0;
    }
    p++;
    v = strtoull(p, &end, 10);
    (void)end;
    return v;
}

static int
json_hex(const char *line, const char *key, uint8_t *out)
{
    const char *p = strstr(line, key);
    char buf[129];
    uint32_t n = 0;

    if (p == NULL) {
        return -1;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return -1;
    }
    p = strchr(p, '"');
    if (p == NULL) {
        return -1;
    }
    p++;
    while (n < 128u && p[n] != 0 && p[n] != '"') {
        buf[n] = p[n];
        n++;
    }
    buf[n] = 0;
    if (n != 128u) {
        return -1;
    }
    return unhex64(buf, out);
}

static int
load_probes(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[LINE_MAX];

    if (f == NULL) {
        return -1;
    }
    while (g_npr < PROBE_MAX && fgets(line, sizeof(line), f) != NULL) {
        probe_t *p = &g_pr[g_npr];

        memset(p, 0, sizeof(*p));
        p->slot = json_u64(line, "\"slot\"");
        if (p->slot == 0 || json_hex(line, "\"sig_hex\"", p->sig) != 0) {
            continue;
        }
        p->used = 1;
        g_npr++;
    }
    fclose(f);
    return 0;
}

static int
has_sig(const uint8_t *buf, uint32_t n, const uint8_t sig[64])
{
    uint32_t i;

    if (buf == NULL || n < 64u) {
        return 0;
    }
    for (i = 0; i + 64u <= n; i++) {
        if (memcmp(buf + i, sig, 64) == 0) {
            return 1;
        }
    }
    return 0;
}

static bag_t *
bag_of(uint64_t slot, int create)
{
    uint32_t i, free_i = BAG_MAX, oldest_i = BAG_MAX;
    uint64_t oldest = UINT64_MAX;

    for (i = 0; i < BAG_MAX; i++) {
        if (g_bag[i].used && g_bag[i].slot == slot) {
            return &g_bag[i];
        }
        if (!g_bag[i].used && free_i == BAG_MAX) {
            free_i = i;
        }
        if (g_bag[i].used && g_bag[i].slot < oldest) {
            oldest = g_bag[i].slot;
            oldest_i = i;
        }
    }
    if (!create) {
        return NULL;
    }
    if (free_i >= BAG_MAX) {
        return oldest_i < BAG_MAX ? &g_bag[oldest_i] : NULL;
    }
    g_bag[free_i].used = 1;
    g_bag[free_i].slot = slot;
    g_bag[free_i].n = 0;
    g_bag[free_i].n_data = 0;
    g_bag[free_i].n_code = 0;
    g_bag[free_i].overflow = 0;
    return &g_bag[free_i];
}

static void
classify_slot(bag_t *b)
{
    shred_prefix_info_t id = { 0 }, iff = { 0 };
    shred_fec_rec_t rec[SHRED_FEC_OUT_MAX];
    uint32_t i, p, nrec;
    const uint8_t *cd = NULL, *cf = NULL;
    uint32_t nd = 0, nf = 0;
    uint32_t n_data_push = 0;

    if (b == NULL || !b->used) {
        return;
    }
    g_slots_seen++;
    shred_prefix_free(g_px_data);
    shred_prefix_free(g_px_fec);
    g_px_data = NULL;
    g_px_fec = NULL;
    if (shred_prefix_init(&g_px_data) != 0
        || shred_prefix_init(&g_px_fec) != 0) {
        return;
    }
    (void)shred_prefix_watch(g_px_data, b->slot);
    (void)shred_prefix_watch(g_px_fec, b->slot);
    for (i = 0; i < b->n; i++) {
        const bag_shred_t *s = &b->sh[i];

        if (!s->used) {
            continue;
        }
        nrec = 0;
        (void)shred_fec_push(g_fec, s->pkt, s->len, 1, rec, SHRED_FEC_OUT_MAX,
                             &nrec);
        for (p = 0; p < nrec; p++) {
            (void)shred_prefix_push(g_px_fec, rec[p].pkt, rec[p].len);
        }
        if (!s->is_code) {
            (void)shred_prefix_push(g_px_data, s->pkt, s->len);
            (void)shred_prefix_push(g_px_fec, s->pkt, s->len);
            n_data_push++;
        }
    }
    (void)shred_prefix_info(g_px_data, b->slot, &id);
    (void)shred_prefix_info(g_px_fec, b->slot, &iff);
    (void)shred_prefix_bytes(g_px_data, b->slot, &cd, &nd);
    (void)shred_prefix_bytes(g_px_fec, b->slot, &cf, &nf);
    if (id.first_hole != 0) {
        g_slots_hole++;
    }
    if (iff.first_hole > id.first_hole) {
        g_slots_fec_gain++;
    }
    for (p = 0; p < g_npr; p++) {
        uint8_t in_d, in_f, in_raw;
        uint32_t k;

        if (!g_pr[p].used || g_pr[p].slot != b->slot || g_pr[p].cls != 0) {
            continue;
        }
        in_d = (uint8_t)has_sig(cd, nd, g_pr[p].sig);
        in_f = (uint8_t)has_sig(cf, nf, g_pr[p].sig);
        in_raw = 0;
        for (k = 0; k < b->n && !in_raw; k++) {
            if (!b->sh[k].used || b->sh[k].is_code) {
                continue;
            }
            in_raw = (uint8_t)has_sig(b->sh[k].pkt, b->sh[k].len,
                                      g_pr[p].sig);
        }
        if (in_d) {
            g_pr[p].cls = CLS_A;
            g_n_a++;
        } else if (in_f) {
            g_pr[p].cls = CLS_B;
            g_n_b++;
        } else {
            g_pr[p].cls = CLS_C;
            g_n_c++;
            if (in_raw) {
                g_n_orphan++;
            }
        }
    }
    fprintf(stdout,
            "{\"slot\":%" PRIu64 ",\"n_data\":%" PRIu32 ",\"n_code\":%" PRIu32
            ",\"overflow\":%" PRIu32 ",\"hole_data\":%" PRIu32
            ",\"hole_fec\":%" PRIu32 ",\"concat_data\":%" PRIu32
            ",\"concat_fec\":%" PRIu32 ",\"n_batch_data\":%" PRIu32
            ",\"n_batch_fec\":%" PRIu32 "}\n",
            b->slot, b->n_data, b->n_code, b->overflow, id.first_hole,
            iff.first_hole, id.concat_len, iff.concat_len, id.n_batch,
            iff.n_batch);
    b->used = 0;
    b->n = 0;
    (void)n_data_push;
}

static void
flush_old(uint64_t now)
{
    uint32_t i;

    for (i = 0; i < BAG_MAX; i++) {
        if (g_bag[i].used && now > g_bag[i].slot + 256ull) {
            classify_slot(&g_bag[i]);
        }
    }
}

static int
bag_add(bag_t *b, const uint8_t *sp, uint16_t slen, const shred_view_t *v)
{
    uint32_t i;
    bag_shred_t *s;
    int code = shred_is_code(v->type);

    for (i = 0; i < b->n; i++) {
        if (b->sh[i].used && b->sh[i].is_code == (uint8_t)code
            && b->sh[i].index == v->index) {
            return 0;
        }
    }
    if (b->n >= SHRED_CAP) {
        b->overflow++;
        return 0;
    }
    s = &b->sh[b->n++];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->is_code = (uint8_t)code;
    s->index = v->index;
    s->len = slen > SHRED_MAX_SZ ? (uint16_t)SHRED_MAX_SZ : slen;
    memcpy(s->pkt, sp, s->len);
    if (code) {
        b->n_code++;
    } else {
        b->n_data++;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    const char *cap_path = NULL, *probe_path = NULL;
    capio_t cap;
    uint32_t i;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--cap") == 0 && i + 1 < (uint32_t)argc) {
            cap_path = argv[++i];
        } else if (strcmp(argv[i], "--probes") == 0
                   && i + 1 < (uint32_t)argc) {
            probe_path = argv[++i];
        }
    }
    if (cap_path == NULL || probe_path == NULL) {
        fprintf(stderr, "usage: shred_continuity --cap FILE --probes JSONL\n");
        return 2;
    }
    if (load_probes(probe_path) != 0) {
        fprintf(stderr, "probes open failed\n");
        return 1;
    }
    if (shred_fec_init(&g_fec) != 0) {
        fprintf(stderr, "fec init failed\n");
        return 1;
    }
    if (capio_open(&cap, cap_path) != 0) {
        fprintf(stderr, "cap open failed\n");
        return 1;
    }
    fprintf(stderr, "probes=%u\n", g_npr);
    while (1) {
        net_slot_t sl;
        shred_view_t v;
        shred_identity_t id;
        bag_t *b;
        int rc = capio_read(&cap, &sl);

        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fprintf(stderr, "cap read error\n");
            break;
        }
        if (shred_identify_packet(sl.data, sl.len, &v, &id) != 0) {
            continue;
        }
        if (id.slot <= FROM_SLOT || id.slot > TO_SLOT) {
            continue;
        }
        flush_old(id.slot);
        b = bag_of(id.slot, 1);
        if (b == NULL) {
            continue;
        }
        if (b->slot != id.slot) {
            classify_slot(b);
            b->used = 1;
            b->slot = id.slot;
            b->n = 0;
            b->n_data = 0;
            b->n_code = 0;
            b->overflow = 0;
        }
        {
            const uint8_t *sp;
            uint16_t slen;

            if (shred_envelope(sl.data, sl.len, &sp, &slen) == 0) {
                (void)bag_add(b, sp, slen, &v);
            }
        }
    }
    for (i = 0; i < BAG_MAX; i++) {
        if (g_bag[i].used) {
            classify_slot(&g_bag[i]);
        }
    }
    capio_close(&cap);
    {
        uint32_t leftover = 0;

        for (i = 0; i < g_npr; i++) {
            if (g_pr[i].used && g_pr[i].cls == 0) {
                g_pr[i].cls = CLS_C;
                g_n_c++;
                leftover++;
            }
        }
        fprintf(stderr, "leftover_unseen_slots=%u\n", leftover);
    }
    fprintf(stderr,
            "SHRED-CONTINUITY-001  probes=%u\n"
            "A RAW_PRESENT          %" PRIu64 "\n"
            "B FEC_RECOVERABLE      %" PRIu64 "\n"
            "C FEED_MISSING         %" PRIu64 "\n"
            "  of C, raw_orphan     %" PRIu64 "\n"
            "slots_classified       %" PRIu64 "\n"
            "slots_data_hole        %" PRIu64 "\n"
            "slots_fec_extends      %" PRIu64 "\n",
            g_npr, g_n_a, g_n_b, g_n_c, g_n_orphan, g_slots_seen, g_slots_hole,
            g_slots_fec_gain);
    shred_prefix_free(g_px_data);
    shred_prefix_free(g_px_fec);
    shred_fec_free(g_fec);
    return 0;
}
