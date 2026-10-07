#include "shred/assemble.h"

#include "ingress/decode.h"
#include "shred/complete.h"
#include "shred/shred.h"

#include <stdlib.h>
#include <string.h>

#define ASSEM_SLOT_CAP 8u
#define ASSEM_IDX_CAP  32768u
#define ASSEM_Q_CAP    16u

typedef struct {
    uint32_t index;
    uint32_t fec;
    uint16_t plen;
    uint8_t  flags;
    uint8_t  used;
    uint8_t  consumed;
    uint8_t  recovered;
    uint64_t tsc;
    uint64_t tsc_enough;
    uint8_t  payload[1200];
} assem_row_t;

typedef struct {
    uint64_t     slot;
    uint8_t      live;
    uint32_t     n_used;
    uint32_t     used_ix[ASSEM_IDX_CAP];
    assem_row_t *row;
} assem_slot_t;

struct shred_assem {
    assem_slot_t  sl[ASSEM_SLOT_CAP];
    uint32_t      victim;
    shred_fec_t  *fec;
    shred_batch_t q[ASSEM_Q_CAP];
    uint32_t      qh;
    uint32_t      qt;
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
    s->live = 0;
    s->slot = 0;
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
        if (a->sl[i].row == NULL) {
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
            if (r->used && !r->consumed && r->plen != 0) {
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

static int
push_tx(shred_batch_t *b, uint32_t off, uint32_t rem)
{
    ordered_tx_t tx;
    int rc;

    rc = ingress_decode_tx(b->buf + off, rem, b->slot, &tx);
    if (rc != DEC_OK && rc != DEC_ALT) {
        return -1;
    }
    if (tx.raw_len == 0 || tx.raw_len > rem) {
        return -1;
    }
    if (b->n_tx >= ASSEM_TX_MAX) {
        return 0;
    }
    b->tx_off[b->n_tx] = off;
    b->tx_len[b->n_tx] = tx.raw_len;
    b->n_tx++;
    return (int)tx.raw_len;
}

static int
extract_txs(shred_batch_t *b)
{
    uint32_t off = 0;

    b->n_tx = 0;
    while (off < b->blen && b->n_tx < ASSEM_TX_MAX) {
        uint64_t ntx = 0;
        uint32_t rem = b->blen - off;
        int n;

        if (rem >= 48u) {
            memcpy(&ntx, b->buf + off + 40, 8);
            if (ntx == 0) {
                off += 48;
                continue;
            }
            if (ntx < 64u) {
                uint64_t t;

                off += 48;
                for (t = 0; t < ntx && b->n_tx < ASSEM_TX_MAX && off < b->blen;
                     t++) {
                    n = push_tx(b, off, b->blen - off);
                    if (n <= 0) {
                        return (b->n_tx != 0) ? 1 : 0;
                    }
                    off += (uint32_t)n;
                }
                continue;
            }
        }
        n = push_tx(b, off, rem);
        if (n <= 0) {
            break;
        }
        off += (uint32_t)n;
    }
    return (b->n_tx != 0) ? 1 : 0;
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
try_complete(shred_assem_t *a, uint64_t slot, uint32_t end,
             shred_batch_t *batch)
{
    uint32_t start, i, n = 0, fec_n = 0;
    uint64_t tfirst = 0, tdone = 0, tenough = 0;

    start = end;
    while (start > 0) {
        assem_row_t *p = find_row(a, slot, start - 1u, 0);
        if (p == NULL || !p->used || p->consumed) {
            break;
        }
        start--;
        if ((p->flags & SHRED_DATA_COMPLETE) != 0 && start != end) {
            start++;
            break;
        }
    }
    memset(batch, 0, sizeof(*batch));
    batch->slot = slot;
    for (i = start; i <= end; i++) {
        assem_row_t *r = find_row(a, slot, i, 0);
        if (r == NULL || !r->used || r->consumed) {
            return 0;
        }
        if (batch->blen + r->plen > ASSEM_BUF_MAX) {
            return 0;
        }
        memcpy(batch->buf + batch->blen, r->payload, r->plen);
        batch->blen += r->plen;
        if (tfirst == 0 || r->tsc < tfirst) {
            tfirst = r->tsc;
        }
        if (r->tsc > tdone) {
            tdone = r->tsc;
        }
        if (r->tsc_enough > tdone) {
            tdone = r->tsc_enough;
        }
        if (r->recovered) {
            fec_n++;
            if (r->tsc_enough > tenough) {
                tenough = r->tsc_enough;
            }
        }
        n++;
    }
    if (n == 0) {
        return 0;
    }
    for (i = start; i <= end; i++) {
        assem_row_t *r = find_row(a, slot, i, 0);
        if (r != NULL) {
            r->consumed = 1;
        }
    }
    batch->n_shreds = n;
    if (fec_n != 0) {
        batch->how = RECON_FEC;
        batch->fec_possible = 1;
    } else {
        batch->how = (n == 1) ? RECON_ONE : RECON_MULTI;
    }
    batch->tsc_first = tfirst;
    batch->tsc_enough = tenough != 0 ? tenough : tdone;
    batch->tsc_done = tdone;
    (void)extract_txs(batch);
    return 1;
}

static int
ingest_data(shred_assem_t *a, const uint8_t *pkt, uint16_t len,
            uint64_t rx_tsc, uint64_t tsc_enough, int recovered)
{
    shred_view_t v;
    assem_row_t *r;
    shred_batch_t b;
    uint16_t size, body;

    if (shred_parse(pkt, len, &v) != 0 || !shred_is_data(v.type)
        || len < SHRED_DATA_HDR_SZ) {
        return 0;
    }
    r = find_row(a, v.slot, v.index, 1);
    if (r == NULL || r->plen != 0) {
        return 0;
    }
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
    if ((r->flags & SHRED_DATA_COMPLETE) != 0
        && try_complete(a, v.slot, v.index, &b) == 1) {
        (void)q_push(a, &b);
        return 1;
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
    if (shred_is_data(v.type)) {
        (void)ingest_data(a, pkt, len, rx_tsc, 0, 0);
    }
    for (i = 0; i < nrec; i++) {
        (void)ingest_data(a, rec[i].pkt, rec[i].len, rec[i].tsc_first,
                          rec[i].tsc_enough, 1);
    }
    return q_pop(a, batch);
}
