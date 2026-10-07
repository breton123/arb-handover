#ifndef FIRM_NET_RING_H
#define FIRM_NET_RING_H

#include "packet.h"

#include <stdatomic.h>
#include <stdint.h>

/*
 * Power-of-two SPSC slot ring.
 *
 * Ownership
 *   created/freed: application at startup/shutdown
 *   writer:        one RX thread (or test inject)
 *   reader:        one consumer thread
 *   slot lifetime: writer fills after claim, before commit;
 *                  reader holds the payload pointer from acquire until release
 *
 * No mutex. No malloc after init. Full ring drops (lossy); writer never waits.
 *
 * claim/commit lets recvmmsg write directly into slot payloads.
 * One outstanding acquire per reader. Peek does not take ownership.
 */

typedef struct {
    net_slot_t      *slot;
    uint32_t         mask;
    uint32_t         claimed; /* writer-local; not published */
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    _Atomic uint64_t drops;
    _Atomic uint64_t trunc;
} net_ring_t;

int net_ring_init(net_ring_t *r, uint32_t slots);
void net_ring_free(net_ring_t *r);

uint32_t net_ring_claim(net_ring_t *r, net_slot_t **out, uint32_t n);
void net_ring_commit(net_ring_t *r, uint32_t n);
void net_ring_abort(net_ring_t *r);

int net_ring_push(net_ring_t *r, const net_packet_t *pkt);
int net_ring_peek(const net_ring_t *r, net_packet_t *out);
int net_ring_acquire(net_ring_t *r, net_packet_t *out);
void net_ring_release(net_ring_t *r);

/* Copy-out pop for off-path consumers (capture recorder). */
int net_ring_pop_copy(net_ring_t *r, net_slot_t *out);

static inline uint64_t
net_ring_depth(const net_ring_t *r)
{
    uint64_t head, tail;

    if (r == NULL) {
        return 0;
    }
    head = atomic_load_explicit(&r->head, memory_order_acquire);
    tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    return head - tail;
}

static inline uint32_t
net_ring_capacity(const net_ring_t *r)
{
    if (r == NULL) {
        return 0;
    }
    return r->mask + 1u;
}

#endif /* FIRM_NET_RING_H */
