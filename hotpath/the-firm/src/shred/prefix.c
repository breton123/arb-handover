#include "shred/prefix.h"

#include "shred/complete.h"
#include "shred/shred.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t index;
    uint16_t plen;
    uint8_t  used;
    uint8_t  flags;
    uint8_t  payload[1200];
} prefix_row_t;

typedef struct {
    uint8_t      used;
    uint64_t     slot;
    uint32_t     n;
    uint32_t     n_batch;
    uint32_t     concat_len;
    uint32_t     batch_end[PREFIX_BATCH_MAX];
    uint8_t     *concat;
    prefix_row_t row[PREFIX_ROW_MAX];
} prefix_slot_t;

struct shred_prefix {
    prefix_slot_t sl[PREFIX_SLOT_MAX];
};

static prefix_slot_t *
slot_of(shred_prefix_t *p, uint64_t slot, int create)
{
    uint32_t i, free_i = PREFIX_SLOT_MAX;

    for (i = 0; i < PREFIX_SLOT_MAX; i++) {
        if (p->sl[i].used && p->sl[i].slot == slot) {
            return &p->sl[i];
        }
        if (!p->sl[i].used && free_i == PREFIX_SLOT_MAX) {
            free_i = i;
        }
    }
    if (!create || free_i >= PREFIX_SLOT_MAX) {
        return NULL;
    }
    p->sl[free_i].used = 1;
    p->sl[free_i].slot = slot;
    p->sl[free_i].n = 0;
    p->sl[free_i].concat_len = 0;
    if (p->sl[free_i].concat == NULL) {
        p->sl[free_i].concat = calloc(1, PREFIX_BUF);
        if (p->sl[free_i].concat == NULL) {
            p->sl[free_i].used = 0;
            return NULL;
        }
    }
    return &p->sl[free_i];
}

static const prefix_slot_t *
slot_get(const shred_prefix_t *p, uint64_t slot)
{
    uint32_t i;

    for (i = 0; i < PREFIX_SLOT_MAX; i++) {
        if (p->sl[i].used && p->sl[i].slot == slot) {
            return &p->sl[i];
        }
    }
    return NULL;
}

static void
rebuild(prefix_slot_t *s)
{
    uint32_t hole = 0, i;

    s->concat_len = 0;
    s->n_batch = 0;
    for (;;) {
        const prefix_row_t *hit = NULL;

        for (i = 0; i < s->n; i++) {
            if (s->row[i].used && s->row[i].index == hole) {
                hit = &s->row[i];
                break;
            }
        }
        if (hit == NULL) {
            break;
        }
        if (s->concat_len + hit->plen > PREFIX_BUF) {
            break;
        }
        memcpy(s->concat + s->concat_len, hit->payload, hit->plen);
        s->concat_len += hit->plen;
        if ((hit->flags & PREFIX_FLAG_DATA_COMPLETE) != 0
            && s->n_batch < PREFIX_BATCH_MAX) {
            s->batch_end[s->n_batch++] = s->concat_len;
        }
        hole++;
    }
}

int
shred_prefix_init(shred_prefix_t **out)
{
    shred_prefix_t *p;

    if (out == NULL) {
        return -1;
    }
    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return -1;
    }
    *out = p;
    return 0;
}

void
shred_prefix_free(shred_prefix_t *p)
{
    uint32_t i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < PREFIX_SLOT_MAX; i++) {
        free(p->sl[i].concat);
    }
    free(p);
}

int
shred_prefix_watch(shred_prefix_t *p, uint64_t slot)
{
    return (p != NULL && slot_of(p, slot, 1) != NULL) ? 0 : -1;
}

int
shred_prefix_push(shred_prefix_t *p, const uint8_t *pkt, uint16_t len)
{
    const uint8_t *sp;
    uint16_t slen;
    shred_view_t v;
    prefix_slot_t *s;
    prefix_row_t *r;
    uint16_t size, body;
    uint32_t i;

    if (p == NULL || pkt == NULL || shred_envelope(pkt, len, &sp, &slen) != 0) {
        return 0;
    }
    if (shred_parse(sp, slen, &v) != 0 || !shred_is_data(v.type)
        || slen < SHRED_DATA_HDR_SZ) {
        return 0;
    }
    s = slot_of(p, v.slot, 0);
    if (s == NULL) {
        return 0;
    }
    for (i = 0; i < s->n; i++) {
        if (s->row[i].used && s->row[i].index == v.index) {
            return 0;
        }
    }
    if (s->n >= PREFIX_ROW_MAX) {
        return 0;
    }
    r = &s->row[s->n++];
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->index = v.index;
    r->flags = (slen > SHRED_OFF_FLAGS) ? sp[SHRED_OFF_FLAGS] : 0;
    size = shred_load_u16_le(sp + SHRED_OFF_SIZE);
    body = (uint16_t)(slen - SHRED_DATA_HDR_SZ);
    if (size >= SHRED_DATA_HDR_SZ && size <= slen) {
        body = (uint16_t)(size - SHRED_DATA_HDR_SZ);
    } else if (size != 0 && size < body) {
        body = size;
    }
    if (body > sizeof(r->payload)) {
        body = (uint16_t)sizeof(r->payload);
    }
    r->plen = body;
    memcpy(r->payload, sp + SHRED_DATA_HDR_SZ, body);
    rebuild(s);
    return 0;
}

int
shred_prefix_info(const shred_prefix_t *p, uint64_t slot,
                  shred_prefix_info_t *out)
{
    const prefix_slot_t *s;
    uint32_t hole = 0, i, have0 = 0;

    if (p == NULL || out == NULL) {
        return -1;
    }
    s = slot_get(p, slot);
    if (s == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->slot = slot;
    out->n_data = s->n;
    out->n_batch = s->n_batch;
    out->concat_len = s->concat_len;
    for (i = 0; i < s->n; i++) {
        if (s->row[i].used && s->row[i].index == 0) {
            have0 = 1;
        }
    }
    out->have0 = (uint8_t)have0;
    for (;;) {
        int hit = 0;

        for (i = 0; i < s->n; i++) {
            if (s->row[i].used && s->row[i].index == hole) {
                hit = 1;
                break;
            }
        }
        if (!hit) {
            break;
        }
        hole++;
    }
    out->first_hole = hole;
    return 0;
}

int
shred_prefix_bytes(const shred_prefix_t *p, uint64_t slot, const uint8_t **buf,
                   uint32_t *len)
{
    const prefix_slot_t *s = (p != NULL) ? slot_get(p, slot) : NULL;

    if (s == NULL || buf == NULL || len == NULL) {
        return -1;
    }
    *buf = s->concat;
    *len = s->concat_len;
    return 0;
}

int
shred_prefix_batches(const shred_prefix_t *p, uint64_t slot,
                     uint32_t *end_off, uint32_t cap, uint32_t *n)
{
    const prefix_slot_t *s = (p != NULL) ? slot_get(p, slot) : NULL;
    uint32_t i, m;

    if (s == NULL || end_off == NULL || n == NULL) {
        return -1;
    }
    m = s->n_batch;
    if (m > cap) {
        m = cap;
    }
    for (i = 0; i < m; i++) {
        end_off[i] = s->batch_end[i];
    }
    *n = m;
    return 0;
}
