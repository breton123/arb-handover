#include "net/ring.h"

#include <string.h>
#include <stdlib.h>

int
net_ring_init(net_ring_t *r, uint32_t slots)
{
    if (r == NULL || slots < 2u || (slots & (slots - 1u)) != 0u) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->slot = calloc((size_t)slots, sizeof(net_slot_t));
    if (r->slot == NULL) {
        return -1;
    }
    r->mask = slots - 1u;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    return 0;
}

void
net_ring_free(net_ring_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->slot);
    r->slot = NULL;
}

uint32_t
net_ring_claim(net_ring_t *r, net_slot_t **out, uint32_t n)
{
    uint64_t head, tail, free_n;
    uint32_t i, take;

    if (r == NULL || out == NULL || n == 0u) {
        return 0;
    }
    head = atomic_load_explicit(&r->head, memory_order_relaxed);
    tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    free_n = (uint64_t)r->mask + 1u - (head - tail);
    take = n;
    if ((uint64_t)take > free_n) {
        take = (uint32_t)free_n;
    }
    for (i = 0; i < take; i++) {
        out[i] = &r->slot[(head + (uint64_t)i) & (uint64_t)r->mask];
    }
    r->claimed = take;
    return take;
}

void
net_ring_commit(net_ring_t *r, uint32_t n)
{
    uint64_t head;

    if (r == NULL || n == 0u) {
        return;
    }
    if (n > r->claimed) {
        n = r->claimed;
    }
    head = atomic_load_explicit(&r->head, memory_order_relaxed);
    atomic_store_explicit(&r->head, head + (uint64_t)n, memory_order_release);
    r->claimed = 0;
}

void
net_ring_abort(net_ring_t *r)
{
    if (r == NULL) {
        return;
    }
    r->claimed = 0;
}

static void
fill_slot(net_ring_t *r, net_slot_t *s, const net_packet_t *pkt)
{
    uint32_t n;

    n = pkt->len;
    s->flags = pkt->flags;
    if (n > NET_PKT_MAX) {
        n = NET_PKT_MAX;
        s->flags = (uint8_t)(s->flags | NET_PKT_TRUNC);
        atomic_fetch_add_explicit(&r->trunc, 1, memory_order_relaxed);
    }
    s->rx_ns = pkt->rx_ns;
    s->rx_tsc = pkt->rx_tsc;
    s->seq = pkt->seq;
    s->len = (uint16_t)n;
    s->source_id = pkt->source_id;
    if (n != 0u && pkt->data != NULL) {
        memcpy(s->data, pkt->data, n);
    }
}

int
net_ring_push(net_ring_t *r, const net_packet_t *pkt)
{
    net_slot_t *s;

    if (r == NULL || pkt == NULL || pkt->data == NULL) {
        return -1;
    }
    if (net_ring_claim(r, &s, 1u) != 1u) {
        atomic_fetch_add_explicit(&r->drops, 1, memory_order_relaxed);
        return 1;
    }
    fill_slot(r, s, pkt);
    net_ring_commit(r, 1u);
    return 0;
}

static int
view_at_tail(const net_ring_t *r, net_packet_t *out)
{
    uint64_t head, tail;
    const net_slot_t *s;

    tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head) {
        return 1;
    }
    s = &r->slot[tail & (uint64_t)r->mask];
    out->data = s->data;
    out->len = s->len;
    out->source_id = s->source_id;
    out->flags = s->flags;
    out->seq = s->seq;
    out->rx_ns = s->rx_ns;
    out->rx_tsc = s->rx_tsc;
    return 0;
}

int
net_ring_peek(const net_ring_t *r, net_packet_t *out)
{
    if (r == NULL || out == NULL) {
        return -1;
    }
    return view_at_tail(r, out);
}

int
net_ring_acquire(net_ring_t *r, net_packet_t *out)
{
    if (r == NULL || out == NULL) {
        return -1;
    }
    return view_at_tail(r, out);
}

void
net_ring_release(net_ring_t *r)
{
    uint64_t tail;

    if (r == NULL) {
        return;
    }
    tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    atomic_store_explicit(&r->tail, tail + 1u, memory_order_release);
}

int
net_ring_pop_copy(net_ring_t *r, net_slot_t *out)
{
    uint64_t head, tail;

    if (r == NULL || out == NULL) {
        return -1;
    }
    tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head) {
        return 1;
    }
    memcpy(out, &r->slot[tail & (uint64_t)r->mask], sizeof(*out));
    atomic_store_explicit(&r->tail, tail + 1u, memory_order_release);
    return 0;
}
