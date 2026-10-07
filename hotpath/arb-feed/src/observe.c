#include "classify.h"
#include "feed.h"
#include "shred.h"

void
feed_observe(feed_stats_t *st, const rx_packet_t *pkt)
{
    shred_view_t v;
    uint32_t rel;
    uint16_t plen;

    if (st == NULL || pkt == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&st->rx, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->bytes, pkt->len, memory_order_relaxed);
    if (shred_parse(pkt->data, pkt->len, &v) != 0) {
        atomic_fetch_add_explicit(&st->bad, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&st->shred_ok, 1, memory_order_relaxed);
    if (shred_is_data(v.type)) {
        atomic_fetch_add_explicit(&st->data, 1, memory_order_relaxed);
    } else if (shred_is_code(v.type)) {
        atomic_fetch_add_explicit(&st->coding, 1, memory_order_relaxed);
    }
    plen = (uint16_t)(pkt->len - (uint16_t)(v.payload - pkt->data));
    rel = classify_have_avx2() ? classify_avx2(v.payload, plen)
                               : classify_scalar(v.payload, plen);
    if (rel != REL_NONE) {
        atomic_fetch_add_explicit(&st->relevant, 1, memory_order_relaxed);
    }
    if (rel & REL_DLMM) {
        atomic_fetch_add_explicit(&st->dlmm, 1, memory_order_relaxed);
    }
    if (rel & REL_PUMP) {
        atomic_fetch_add_explicit(&st->pump, 1, memory_order_relaxed);
    }
}

void
feed_stats_snapshot(const feed_stats_t *st, feed_stats_t *out)
{
    if (st == NULL || out == NULL) {
        return;
    }
    out->rx       = atomic_load_explicit(&st->rx, memory_order_relaxed);
    out->bytes    = atomic_load_explicit(&st->bytes, memory_order_relaxed);
    out->bad      = atomic_load_explicit(&st->bad, memory_order_relaxed);
    out->shred_ok = atomic_load_explicit(&st->shred_ok, memory_order_relaxed);
    out->data     = atomic_load_explicit(&st->data, memory_order_relaxed);
    out->coding   = atomic_load_explicit(&st->coding, memory_order_relaxed);
    out->relevant = atomic_load_explicit(&st->relevant, memory_order_relaxed);
    out->dlmm     = atomic_load_explicit(&st->dlmm, memory_order_relaxed);
    out->pump     = atomic_load_explicit(&st->pump, memory_order_relaxed);
}
