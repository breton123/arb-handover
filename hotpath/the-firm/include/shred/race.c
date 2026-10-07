#include "shred/race.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t          used;
    uint8_t          first_source;
    uint8_t          seen_mask;
    shred_identity_t id;
    uint64_t         first_rx_ns;
    uint64_t         first_rx_tsc;
} shred_race_ent_t;

typedef struct {
    uint8_t first;
    uint8_t late;
    int64_t delta_ns;
} shred_race_sample_t;

struct shred_race {
    shred_race_ent_t    *ent;
    uint32_t             mask;
    uint32_t             sample_cap;
    uint32_t             sample_head;
    uint32_t             sample_n;
    shred_race_sample_t *sample;
    shred_race_stats_t   st;
};

static uint64_t
shred_id_hash(const shred_identity_t *id)
{
    uint64_t h = id->slot + 0x9e3779b97f4a7c15ULL;
    h ^= (uint64_t)id->index * 0xbf58476d1ce4e5b9ULL;
    h ^= (uint64_t)id->fec_set * 0x94d049bb133111ebULL;
    h ^= (uint64_t)id->shred_type * 0x517cc1b727220a95ULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static int
ent_stale(const shred_race_ent_t *e, uint64_t slot)
{
    if (e->used == 0u) {
        return 1;
    }
    if (slot > e->id.slot && (slot - e->id.slot) >= SHRED_RACE_SLOT_LAG) {
        return 1;
    }
    return 0;
}

static void
note_dup(shred_race_t *t, uint8_t first, uint8_t late, int64_t delta)
{
    uint32_t i;

    if (first >= NET_SOURCE_MAX || late >= NET_SOURCE_MAX) {
        return;
    }
    t->st.late[late]++;
    if (t->st.pair_n[first][late] == 0) {
        t->st.pair_min[first][late] = delta;
        t->st.pair_max[first][late] = delta;
    } else {
        if (delta < t->st.pair_min[first][late]) {
            t->st.pair_min[first][late] = delta;
        }
        if (delta > t->st.pair_max[first][late]) {
            t->st.pair_max[first][late] = delta;
        }
    }
    t->st.pair_n[first][late]++;

    i = t->sample_head;
    t->sample[i].first = first;
    t->sample[i].late = late;
    t->sample[i].delta_ns = delta;
    t->sample_head = (i + 1u) & (t->sample_cap - 1u);
    if (t->sample_n < t->sample_cap) {
        t->sample_n++;
    }
}

int
shred_race_init(shred_race_t **out, uint32_t cap)
{
    shred_race_t *t;

    if (out == NULL || cap < 2u || (cap & (cap - 1u)) != 0u) {
        return -1;
    }
    t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return -1;
    }
    t->ent = calloc((size_t)cap, sizeof(*t->ent));
    t->sample = calloc(SHRED_RACE_SAMPLE_CAP, sizeof(*t->sample));
    if (t->ent == NULL || t->sample == NULL) {
        shred_race_free(t);
        return -1;
    }
    t->mask = cap - 1u;
    t->sample_cap = SHRED_RACE_SAMPLE_CAP;
    *out = t;
    return 0;
}

void
shred_race_free(shred_race_t *t)
{
    if (t == NULL) {
        return;
    }
    free(t->ent);
    free(t->sample);
    free(t);
}

static void
install_first(shred_race_t *t, shred_race_ent_t *e, const shred_identity_t *id,
              const net_packet_t *pkt)
{
    e->used = 1;
    e->id = *id;
    e->first_source = pkt->source_id;
    e->seen_mask = (uint8_t)(1u << pkt->source_id);
    e->first_rx_ns = pkt->rx_ns;
    e->first_rx_tsc = pkt->rx_tsc;
    t->st.first[pkt->source_id]++;
}

int
shred_race_claim(shred_race_t *t, const net_packet_t *pkt,
                 shred_race_result_t *out)
{
    shred_identity_t id;
    shred_view_t view;
    uint64_t h;
    uint32_t i, p;
    int victim;

    if (t == NULL || pkt == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!net_source_valid(pkt->source_id) || pkt->data == NULL) {
        out->verdict = SHRED_RACE_BAD;
        t->st.bad++;
        return SHRED_RACE_BAD;
    }
    if (shred_identify(pkt->data, pkt->len, &view, &id) != 0) {
        out->verdict = SHRED_RACE_BAD;
        t->st.bad++;
        return SHRED_RACE_BAD;
    }
    out->id = id;
    out->view = view;

    h = shred_id_hash(&id);
    victim = -1;
    for (p = 0; p < SHRED_RACE_PROBE; p++) {
        shred_race_ent_t *e;

        i = (uint32_t)((h + (uint64_t)p) & (uint64_t)t->mask);
        e = &t->ent[i];
        if (e->used && shred_identity_eq(&e->id, &id)) {
            uint8_t bit = (uint8_t)(1u << pkt->source_id);
            int64_t delta = (int64_t)pkt->rx_ns - (int64_t)e->first_rx_ns;

            out->verdict = SHRED_RACE_DUP;
            out->first_source = e->first_source;
            out->late_source = pkt->source_id;
            out->delta_ns = delta;
            if ((e->seen_mask & bit) == 0u) {
                e->seen_mask = (uint8_t)(e->seen_mask | bit);
                note_dup(t, e->first_source, pkt->source_id, delta);
            } else {
                t->st.late[pkt->source_id]++;
            }
            return SHRED_RACE_DUP;
        }
        if (ent_stale(e, id.slot)) {
            if (e->used != 0u) {
                t->st.evict++;
            }
            install_first(t, e, &id, pkt);
            out->verdict = SHRED_RACE_WIN;
            out->first_source = pkt->source_id;
            return SHRED_RACE_WIN;
        }
        if (victim < 0 || t->ent[i].id.slot < t->ent[(uint32_t)victim].id.slot) {
            victim = (int)i;
        }
    }

    /* Table crowded for this hash: evict the oldest probe slot. */
    if (victim >= 0) {
        t->st.evict++;
        install_first(t, &t->ent[victim], &id, pkt);
        out->verdict = SHRED_RACE_WIN;
        out->first_source = pkt->source_id;
        return SHRED_RACE_WIN;
    }
    out->verdict = SHRED_RACE_BAD;
    t->st.bad++;
    return SHRED_RACE_BAD;
}

void
shred_race_stats(const shred_race_t *t, shred_race_stats_t *out)
{
    if (t == NULL || out == NULL) {
        return;
    }
    *out = t->st;
}

static int
cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

int
shred_race_pair_pct(const shred_race_t *t, uint8_t first, uint8_t late,
                    uint64_t *n_out, int64_t *min_ns, int64_t *p50_ns,
                    int64_t *p95_ns, int64_t *max_ns)
{
    uint32_t i, n, cap, start;
    uint32_t take;
    int64_t *scratch;

    if (t == NULL || first >= NET_SOURCE_MAX || late >= NET_SOURCE_MAX) {
        return -1;
    }
    if (n_out != NULL) {
        *n_out = t->st.pair_n[first][late];
    }
    if (min_ns != NULL) {
        *min_ns = t->st.pair_min[first][late];
    }
    if (max_ns != NULL) {
        *max_ns = t->st.pair_max[first][late];
    }
    scratch = calloc(t->sample_cap, sizeof(*scratch));
    if (scratch == NULL) {
        return -1;
    }
    n = 0;
    cap = t->sample_n;
    start = (t->sample_n < t->sample_cap) ? 0u : t->sample_head;
    for (i = 0; i < cap; i++) {
        const shred_race_sample_t *s =
            &t->sample[(start + i) & (t->sample_cap - 1u)];
        if (s->first == first && s->late == late) {
            scratch[n++] = s->delta_ns;
        }
    }
    if (n == 0) {
        free(scratch);
        if (p50_ns != NULL) {
            *p50_ns = 0;
        }
        if (p95_ns != NULL) {
            *p95_ns = 0;
        }
        return 1;
    }
    qsort(scratch, (size_t)n, sizeof(int64_t), cmp_i64);
    take = n - 1u;
    if (p50_ns != NULL) {
        *p50_ns = scratch[(size_t)(0.50 * (double)take)];
    }
    if (p95_ns != NULL) {
        *p95_ns = scratch[(size_t)(0.95 * (double)take)];
    }
    free(scratch);
    return 0;
}
