#if !defined(__linux__)
#error "src/net/feed.c is Linux-only"
#endif

#include "net/feed.h"
#include "net/time.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

static void
prep_iov(net_feed_t *f, uint32_t n, int into_slots)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (into_slots) {
            f->iov[i].iov_base = f->claimed[i]->data;
        } else {
            f->iov[i].iov_base = f->discard[i];
        }
        f->iov[i].iov_len = NET_PKT_MAX;
        memset(&f->msgs[i], 0, sizeof(f->msgs[i]));
        f->msgs[i].msg_hdr.msg_iov = &f->iov[i];
        f->msgs[i].msg_hdr.msg_iovlen = 1;
    }
}

static void
stamp_slot(net_feed_t *f, net_slot_t *s, uint16_t len, uint8_t flags,
           uint64_t rx_ns, uint64_t rx_tsc)
{
    s->rx_ns = rx_ns;
    s->rx_tsc = rx_tsc;
    s->seq = f->seq++;
    s->len = len;
    s->source_id = f->source_id;
    s->flags = flags;
}

static void
maybe_capture(net_feed_t *f, const net_slot_t *s)
{
    net_packet_t pkt;
    int rc;

    if (f->capture == NULL) {
        return;
    }
    pkt.data = s->data;
    pkt.len = s->len;
    pkt.source_id = s->source_id;
    pkt.flags = s->flags;
    pkt.seq = s->seq;
    pkt.rx_ns = s->rx_ns;
    pkt.rx_tsc = s->rx_tsc;
    rc = net_ring_push(f->capture, &pkt);
    if (rc == 1) {
        net_stats_on_cap_drop(f->stats);
    }
}

int
net_feed_open(net_feed_t *f, uint8_t source_id,
              const char *ip, uint16_t port,
              net_ring_t *ingress, net_ring_t *capture,
              net_stats_t *stats, uint32_t batch)
{
    if (f == NULL || !net_source_valid(source_id) || ingress == NULL) {
        return -1;
    }
    if (batch == 0u || batch > NET_RX_BATCH_MAX) {
        return -1;
    }
    memset(f, 0, sizeof(*f));
    f->source_id = source_id;
    f->batch = batch;
    f->ingress = ingress;
    f->capture = capture;
    f->stats = stats;
    f->udp.fd = -1;
    if (net_udp_open(&f->udp, ip, port) != 0) {
        return -1;
    }
    return 0;
}

int
net_orbitflare_open(net_feed_t *f, const char *ip, uint16_t port,
                    net_ring_t *ingress, net_ring_t *capture,
                    net_stats_t *stats, uint32_t batch)
{
    return net_feed_open(f, NET_SOURCE_ORBITFLARE, ip, port,
                         ingress, capture, stats, batch);
}

int
net_feed_set_rcvbuf(net_feed_t *f, int bytes)
{
    if (f == NULL) {
        return -1;
    }
    return net_udp_set_rcvbuf(&f->udp, bytes);
}

int
net_feed_set_busy_poll(net_feed_t *f, int busy_us)
{
    if (f == NULL) {
        return -1;
    }
    return net_udp_set_busy_poll(&f->udp, busy_us, (int)f->batch);
}

int
net_feed_poll(net_feed_t *f, int wait_ms)
{
    uint32_t got, i, take;
    int n;
    uint64_t rx_ns, rx_tsc;

    if (f == NULL || f->udp.fd < 0) {
        return -1;
    }

    if (wait_ms >= 0) {
        struct pollfd pfd = { .fd = f->udp.fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, wait_ms);

        if (pr < 0) {
            if (errno == EINTR) {
                return 0;
            }
            return -1;
        }
        if (pr == 0) {
            return 0;
        }
    }

    take = net_ring_claim(f->ingress, f->claimed, f->batch);
    if (take == 0u) {
        prep_iov(f, f->batch, 0);
        n = recvmmsg(f->udp.fd, f->msgs, (unsigned int)f->batch,
                     MSG_DONTWAIT, NULL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -1;
        }
        net_stats_on_ring_drop(f->stats, (uint32_t)n);
        atomic_fetch_add_explicit(&f->ingress->drops, (uint64_t)n,
                                  memory_order_relaxed);
        return 0;
    }

    prep_iov(f, take, 1);
    n = recvmmsg(f->udp.fd, f->msgs, (unsigned int)take, MSG_DONTWAIT, NULL);
    if (n < 0) {
        net_ring_abort(f->ingress);
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    if (n == 0) {
        net_ring_abort(f->ingress);
        return 0;
    }

    rx_ns = net_now_ns();
    rx_tsc = net_rdtscp();
    got = 0;
    for (i = 0; i < (uint32_t)n; i++) {
        net_slot_t *s = f->claimed[i];
        unsigned int mlen = f->msgs[i].msg_len;
        uint8_t flags = 0;

        if (mlen == 0u) {
            net_stats_on_malformed(f->stats);
            continue;
        }
        if (mlen > NET_PKT_MAX ||
            (f->msgs[i].msg_hdr.msg_flags & MSG_TRUNC) != 0) {
            flags = NET_PKT_TRUNC;
            if (mlen > NET_PKT_MAX) {
                mlen = NET_PKT_MAX;
            }
            net_stats_on_trunc(f->stats);
            atomic_fetch_add_explicit(&f->ingress->trunc, 1,
                                      memory_order_relaxed);
        }
        stamp_slot(f, s, (uint16_t)mlen, flags, rx_ns, rx_tsc);
        net_stats_on_rx(f->stats, f->source_id, (uint16_t)mlen);
        maybe_capture(f, s);
        if (got != i) {
            /* compact valid slots to the front of the claimed window */
            memcpy(f->claimed[got], s, sizeof(*s));
        }
        got++;
    }

    if (got == 0u) {
        net_ring_abort(f->ingress);
        return 0;
    }
    net_ring_commit(f->ingress, got);
    return (int)got;
}

void
net_feed_close(net_feed_t *f)
{
    if (f == NULL) {
        return;
    }
    net_udp_close(&f->udp);
}
