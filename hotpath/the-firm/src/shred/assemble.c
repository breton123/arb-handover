#include "shred/assemble.h"

#include "shred/complete.h"
#include "shred/shred.h"
#include "shred/stream.h"

#include <stdlib.h>
#include <string.h>

#define ASSEM_SLOT_CAP 8u
#define ASSEM_IDX_CAP  32768u
#define ASSEM_Q_CAP    64u

typedef struct {
    uint32_t index;
    uint32_t fec;
    uint16_t plen;
    uint8_t  flags;
    uint8_t  used;
    uint8_t  consumed;
    uint8_t  promoted;
    uint8_t  recovered;
    uint64_t tsc;
    uint64_t tsc_enough;
    uint8_t  payload[1200];
} assem_row_t;

typedef struct {
    uint64_t     slot;
    uint8_t      live;
    uint32_t     n_used;
    uint32_t     max_ix;
    uint32_t     used_ix[ASSEM_IDX_CAP];
    assem_row_t *row;
    shred_slot_stream_t ss;
} assem_slot_t;

struct shred_assem {
    assem_slot_t  sl[ASSEM_SLOT_CAP];
    uint32_t      victim;
    shred_fec_t  *fec;
    shred_batch_t q[ASSEM_Q_CAP];
    uint32_t      qh;
    uint32_t      qt;
    shred_stream_stats_t st;
};

static void
slot_clear(assem_slot_t *s)
{
    uint32_t i;

    for (i = 0; i < s->n_used; i++) {
        uint32_t ix = s->used_ix[i];
        if (ix < ASSEM_IDX_CAP) {
            memset(&s->row[ix], 0, sizeof(s->row[ix]));
        }
    }
    s->n_used = 0;
    s->max_ix = 0;
    s->live = 0;
    s->slot = 0;
    shred_slot_stream_reset(&s->ss);
}

int
shred_assem_init(shred_assem_t **out)
{
    shred_assem_t *a;
    uint32_t i;

    if (out == NULL) {
        return -1;
    }
    a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return -1;
    }
    for (i = 0; i < ASSEM_SLOT_CAP; i++) {
        a->sl[i].row = calloc(ASSEM_IDX_CAP, sizeof(assem_row_t));
        if (a->sl[i].row == NULL
            || shred_slot_stream_init(&a->sl[i].ss) != 0) {
            shred_assem_free(a);
            return -1;
        }
    }
    if (shred_fec_init(&a->fec) != 0) {
        shred_assem_free(a);
        return -1;
    }
    *out = a;
    return 0;
}

void
shred_assem_free(shred_assem_t *a)
{
    uint32_t i;

    if (a == NULL) {
        return;
    }
    for (i = 0; i < ASSEM_SLOT_CAP; i++) {
        free(a->sl[i].row);
        shred_slot_stream_free(&a->sl[i].ss);
    }
    shred_fec_free(a->fec);
    free(a);
}

uint32_t
shred_assem_incomplete(const shred_assem_t *a)
{
    uint32_t s, i, n = 0;

    if (a == NULL) {
        return 0;
    }
    for (s = 0; s < ASSEM_SLOT_CAP; s++) {
        const assem_slot_t *sl = &a->sl[s];
        if (!sl->live) {
            continue;
        }
        for (i = 0; i < sl->n_used; i++) {
            uint32_t ix = sl->used_ix[i];
            const assem_row_t *r;

            if (ix >= ASSEM_IDX_CAP) {
                continue;
            }
            r = &sl->row[ix];
            if (r->used && !r->promoted && r->plen != 0) {
                n++;
            }
        }
    }
    return n;
}

uint32_t
shred_assem_fec_possible(const shred_assem_t *a)
{
    const shred_fec_stats_t *st;

    if (a == NULL || a->fec == NULL) {
        return 0;
    }
    st = shred_fec_stats(a->fec);
    return st != NULL ? st->possible : 0;
}

const shred_fec_stats_t *
shred_assem_fec_stats(const shred_assem_t *a)
{
    return (a != NULL) ? shred_fec_stats(a->fec) : NULL;
}

const shred_stream_stats_t *
shred_assem_stream_stats(const shred_assem_t *a)
{
    return (a != NULL) ? &a->st : NULL;
}

static int
q_empty(const shred_assem_t *a)
{
    return a->qh == a->qt;
}

static int
q_push(shred_assem_t *a, const shred_batch_t *b)
{
    uint32_t n = (a->qt + 1u) % ASSEM_Q_CAP;
    if (n == a->qh) {
        return -1;
    }
    a->q[a->qt] = *b;
    a->qt = n;
    return 0;
}

static int
q_pop(shred_assem_t *a, shred_batch_t *b)
{
    if (q_empty(a)) {
        return 0;
    }
    *b = a->q[a->qh];
    a->qh = (a->qh + 1u) % ASSEM_Q_CAP;
    return 1;
}

static assem_slot_t *
slot_of(shred_assem_t *a, uint64_t slot, int create)
{
    uint32_t i;
    assem_slot_t *s;

    for (i = 0; i < ASSEM_SLOT_CAP; i++) {
        if (a->sl[i].live && a->sl[i].slot == slot) {
            return &a->sl[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (i = 0; i < ASSEM_SLOT_CAP; i++) {
        if (!a->sl[i].live) {
            a->sl[i].live = 1;
            a->sl[i].slot = slot;
            return &a->sl[i];
        }
    }
    s = &a->sl[a->victim++ % ASSEM_SLOT_CAP];
    slot_clear(s);
    s->live = 1;
    s->slot = slot;
    return s;
}

static assem_row_t *
find_row(shred_assem_t *a, uint64_t slot, uint32_t index, int create)
{
    assem_slot_t *s;
    assem_row_t *r;

    if (index >= ASSEM_IDX_CAP) {
        return NULL;
    }
    s = slot_of(a, slot, create);
    if (s == NULL || s->row == NULL) {
        return NULL;
    }
    r = &s->row[index];
    if (r->used) {
        return r;
    }
    if (!create) {
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->index = index;
    if (s->n_used < ASSEM_IDX_CAP) {
        s->used_ix[s->n_used++] = index;
    }
    return r;
}

static void
refresh_leftover(shred_assem_t *a)
{
    uint32_t i;
    uint64_t n = 0;

    for (i = 0; i < ASSEM_SLOT_CAP; i++) {
        if (a->sl[i].live) {
            n += shred_slot_stream_leftover(&a->sl[i].ss);
        }
    }
    a->st.leftover_bytes = n;
}

static int
flush_stream(shred_assem_t *a, assem_slot_t *sl, int force)
{
    shred_batch_t b;
    uint32_t n_tx = 0, blen = 0, nsh = 0, nfec = 0;
    int rc;

    if (sl->ss.emit_n == 0 && !force) {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.slot = sl->slot;
    b.tsc_first = sl->ss.tsc_first != 0 ? sl->ss.tsc_first : sl->ss.tsc_origin;
    b.tsc_contig = sl->ss.tsc_contig;
    b.tsc_entry = sl->ss.tsc_entry;
    b.tsc_enough = sl->ss.tsc_contig;
    b.tsc_done = sl->ss.tsc_last;
    rc = shred_slot_stream_take(&sl->ss, b.buf, ASSEM_BUF_MAX, b.tx_off,
                                b.tx_len, b.tx_entry, b.tx_ord, ASSEM_TX_MAX,
                                &n_tx, &blen, &nsh, &nfec);
    if (rc < 0) {
        return 0;
    }
    b.n_tx = (uint8_t)n_tx;
    b.blen = blen;
    b.n_shreds = nsh;
    if (nfec != 0) {
        b.how = RECON_FEC;
        b.fec_possible = 1;
    } else {
        b.how = (nsh <= 1u) ? RECON_ONE : RECON_MULTI;
    }
    (void)q_push(a, &b);
    return 1;
}

static void
pump_wm(shred_assem_t *a, assem_slot_t *sl)
{
    int complete = 0;

    if (sl == NULL || sl->row == NULL) {
        return;
    }
    while (sl->ss.wm < ASSEM_IDX_CAP) {
        assem_row_t *r = &sl->row[sl->ss.wm];

        if (!r->used || r->plen == 0 || r->promoted) {
            break;
        }
        if (shred_slot_stream_append(&sl->ss, r->payload, r->plen, r->tsc,
                                     r->recovered, &a->st) != 0) {
            break;
        }
        r->promoted = 1;
        r->consumed = 1;
        if ((r->flags & SHRED_DATA_COMPLETE) != 0) {
            complete = 1;
        }
    }
    do {
        shred_slot_stream_parse(&sl->ss, sl->slot, &a->st);
        if (sl->ss.emit_n == 0) {
            break;
        }
        (void)flush_stream(a, sl, 0);
    } while (sl->ss.emit_n != 0);
    if (complete && sl->ss.emit_n == 0) {
        (void)flush_stream(a, sl, 1);
    }
    refresh_leftover(a);
}

static int
find_shred(const uint8_t *pkt, uint16_t len, const uint8_t **out, uint16_t *olen)
{
    static const uint16_t offs[] = { 0, 8, 28, 42 };
    uint32_t i;

    for (i = 0; i < 4u; i++) {
        shred_view_t v;
        uint16_t o = offs[i];
        uint16_t rem;

        if (len <= o) {
            continue;
        }
        rem = (uint16_t)(len - o);
        if (rem > SHRED_MAX_SZ) {
            rem = (uint16_t)SHRED_MAX_SZ;
        }
        if (shred_parse(pkt + o, rem, &v) == 0) {
            *out = pkt + o;
            *olen = rem;
            return 0;
        }
    }
    return -1;
}

static int
ingest_data(shred_assem_t *a, const uint8_t *pkt, uint16_t len,
            uint64_t rx_tsc, uint64_t tsc_enough, int recovered)
{
    shred_view_t v;
    assem_slot_t *sl;
    assem_row_t *r;
    uint16_t size, body;

    (void)tsc_enough;
    if (shred_parse(pkt, len, &v) != 0 || !shred_is_data(v.type)
        || len < SHRED_DATA_HDR_SZ) {
        return 0;
    }
    r = find_row(a, v.slot, v.index, 1);
    if (r == NULL || r->plen != 0) {
        return 0;
    }
    sl = slot_of(a, v.slot, 0);
    size = shred_load_u16_le(pkt + SHRED_OFF_SIZE);
    body = (uint16_t)(len - SHRED_DATA_HDR_SZ);
    if (size >= SHRED_DATA_HDR_SZ && size <= len) {
        body = (uint16_t)(size - SHRED_DATA_HDR_SZ);
    } else if (size != 0 && size < body) {
        body = size;
    }
    if (body > sizeof(r->payload)) {
        body = (uint16_t)sizeof(r->payload);
    }
    r->fec = v.fec_set;
    r->flags = pkt[SHRED_OFF_FLAGS];
    r->tsc = rx_tsc;
    r->tsc_enough = tsc_enough;
    r->recovered = (uint8_t)(recovered ? 1 : 0);
    r->plen = body;
    memcpy(r->payload, pkt + SHRED_DATA_HDR_SZ, body);
    a->st.auth_shreds++;
    if (sl != NULL) {
        if (v.index > sl->max_ix) {
            sl->max_ix = v.index;
        }
        if (v.index > sl->ss.wm) {
            a->st.gap_blocked++;
        }
        pump_wm(a, sl);
    }
    return 0;
}

int
shred_assem_next(shred_assem_t *a, shred_batch_t *batch)
{
    if (a == NULL || batch == NULL) {
        return 0;
    }
    return q_pop(a, batch);
}

int
shred_assem_push(shred_assem_t *a, const uint8_t *pkt, uint16_t len,
                 uint64_t rx_tsc, shred_batch_t *batch)
{
    shred_view_t v;
    const uint8_t *sp;
    uint16_t slen;
    shred_fec_rec_t rec[SHRED_FEC_OUT_MAX];
    uint32_t nrec = 0, i;

    if (a == NULL || pkt == NULL || batch == NULL) {
        return -1;
    }
    if (find_shred(pkt, len, &sp, &slen) != 0) {
        return -1;
    }
    pkt = sp;
    len = slen;
    if (shred_parse(pkt, len, &v) != 0) {
        return -1;
    }
    (void)shred_fec_push(a->fec, pkt, len, rx_tsc, rec, SHRED_FEC_OUT_MAX,
                         &nrec);
    for (i = 0; i < nrec; i++) {
        (void)ingest_data(a, rec[i].pkt, rec[i].len, rec[i].tsc_first,
                          rec[i].tsc_enough, 1);
    }
    if (shred_is_data(v.type)) {
        (void)ingest_data(a, pkt, len, rx_tsc, 0, 0);
    }
    return q_pop(a, batch);
}
