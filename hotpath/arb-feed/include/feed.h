#ifndef ARB_FEED_FEED_H
#define ARB_FEED_FEED_H

#include "rx.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * arb-feed — UDP (or later AF_XDP) → rx_packet_t.
 * Capture is off-path, lossy, never waits on the trade loop.
 */

#define FEED_PKT_MAX     2048u
#define FEEDCAP_MAGIC    "FEEDCAP1"
#define FEEDCAP_HDR_LEN  40u
#define FEEDCAP_REC_HDR  24u

typedef struct {
    int fd;
} udp_source_t;

int udp_source_open(udp_source_t *s, const char *ip, uint16_t port);
int udp_source_set_rcvbuf(udp_source_t *s, int bytes);
int udp_source_recv(udp_source_t *s, uint8_t *buf, uint16_t cap, rx_packet_t *out);
void udp_source_close(udp_source_t *s);

typedef struct {
    uint64_t rx_ns;
    uint64_t rx_tsc;
    uint32_t len;
    uint32_t seq;
    uint8_t  data[FEED_PKT_MAX];
} feed_slot_t;

typedef struct {
    feed_slot_t     *slot;
    uint32_t         mask;
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    _Atomic uint64_t drops;
    _Atomic uint64_t trunc;
} feed_ring_t;

int feed_ring_init(feed_ring_t *r, uint32_t slots);
void feed_ring_free(feed_ring_t *r);

static inline int
feed_try_push(feed_ring_t *r, const rx_packet_t *pkt,
              uint64_t rx_ns, uint64_t rx_tsc, uint32_t seq)
{
    uint64_t head, tail;
    uint32_t n;
    feed_slot_t *s;

    if (r == NULL || pkt == NULL || pkt->data == NULL) {
        return -1;
    }
    head = atomic_load_explicit(&r->head, memory_order_relaxed);
    tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail >= (uint64_t)r->mask + 1u) {
        atomic_fetch_add_explicit(&r->drops, 1, memory_order_relaxed);
        return 1;
    }
    s = &r->slot[head & r->mask];
    n = pkt->len;
    if (n > FEED_PKT_MAX) {
        n = FEED_PKT_MAX;
        atomic_fetch_add_explicit(&r->trunc, 1, memory_order_relaxed);
    }
    s->rx_ns  = rx_ns;
    s->rx_tsc = rx_tsc;
    s->len    = n;
    s->seq    = seq;
    memcpy(s->data, pkt->data, n);
    atomic_store_explicit(&r->head, head + 1u, memory_order_release);
    return 0;
}

int feed_pop(feed_ring_t *r, feed_slot_t *out);

typedef struct {
    _Atomic uint64_t rx;
    _Atomic uint64_t bytes;
    _Atomic uint64_t bad;
    _Atomic uint64_t shred_ok;
    _Atomic uint64_t data;
    _Atomic uint64_t coding;
    _Atomic uint64_t relevant;
    _Atomic uint64_t dlmm;
    _Atomic uint64_t pump;
} feed_stats_t;

void feed_observe(feed_stats_t *st, const rx_packet_t *pkt);
void feed_stats_snapshot(const feed_stats_t *st, feed_stats_t *out);

typedef struct {
    uint64_t realtime0_ns;
    uint64_t mono0_ns;
    uint64_t tsc_hz;
} feedcap_hdr_t;

int feedcap_write_header(FILE *f, const feedcap_hdr_t *h);
int feedcap_write_slot(FILE *f, const feed_slot_t *s);
int feedcap_read_header(FILE *f, feedcap_hdr_t *h);
int feedcap_read_slot(FILE *f, feed_slot_t *s);

typedef struct recorder recorder_t;

int recorder_start(recorder_t **out, feed_ring_t *ring,
                   const char *dir, const char *prefix,
                   uint64_t rotate_bytes, int rec_cpu,
                   const feedcap_hdr_t *hdr);
void recorder_stop(recorder_t *rec);
uint64_t recorder_written(const recorder_t *rec);
const char *recorder_path(const recorder_t *rec);

#endif /* ARB_FEED_FEED_H */
