#include "net/stats.h"

#include <string.h>

void
net_stats_clear(net_stats_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

static uint64_t
load_u64(const _Atomic uint64_t *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

void
net_stats_snapshot(const net_stats_t *st, net_stats_t *out)
{
    uint32_t i;

    if (st == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->rx = load_u64(&st->rx);
    out->bytes = load_u64(&st->bytes);
    out->malformed = load_u64(&st->malformed);
    out->ring_drops = load_u64(&st->ring_drops);
    out->cap_drops = load_u64(&st->cap_drops);
    out->trunc = load_u64(&st->trunc);
    for (i = 0; i < NET_SOURCE_MAX; i++) {
        out->rx_by_source[i] = load_u64(&st->rx_by_source[i]);
        out->bytes_by_source[i] = load_u64(&st->bytes_by_source[i]);
        out->first_by_source[i] = load_u64(&st->first_by_source[i]);
        out->late_by_source[i] = load_u64(&st->late_by_source[i]);
    }
}

void
net_stats_on_rx(net_stats_t *st, uint8_t source_id, uint16_t len)
{
    if (st == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&st->rx, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->bytes, (uint64_t)len, memory_order_relaxed);
    if (source_id < NET_SOURCE_MAX) {
        atomic_fetch_add_explicit(&st->rx_by_source[source_id], 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&st->bytes_by_source[source_id],
                                  (uint64_t)len, memory_order_relaxed);
    }
}

void
net_stats_on_malformed(net_stats_t *st)
{
    if (st != NULL) {
        atomic_fetch_add_explicit(&st->malformed, 1, memory_order_relaxed);
    }
}

void
net_stats_on_ring_drop(net_stats_t *st, uint32_t n)
{
    if (st != NULL && n != 0u) {
        atomic_fetch_add_explicit(&st->ring_drops, (uint64_t)n,
                                  memory_order_relaxed);
    }
}

void
net_stats_on_cap_drop(net_stats_t *st)
{
    if (st != NULL) {
        atomic_fetch_add_explicit(&st->cap_drops, 1, memory_order_relaxed);
    }
}

void
net_stats_on_trunc(net_stats_t *st)
{
    if (st != NULL) {
        atomic_fetch_add_explicit(&st->trunc, 1, memory_order_relaxed);
    }
}

void
net_stats_on_first(net_stats_t *st, uint8_t source_id)
{
    if (st != NULL && source_id < NET_SOURCE_MAX) {
        atomic_fetch_add_explicit(&st->first_by_source[source_id], 1,
                                  memory_order_relaxed);
    }
}

void
net_stats_on_late(net_stats_t *st, uint8_t source_id)
{
    if (st != NULL && source_id < NET_SOURCE_MAX) {
        atomic_fetch_add_explicit(&st->late_by_source[source_id], 1,
                                  memory_order_relaxed);
    }
}
