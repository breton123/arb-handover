#include "net/sink.h"

#include <string.h>

void
net_sink_clear(net_sink_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

int
net_sink_consume(net_sink_t *s, const net_packet_t *pkt)
{
    if (s == NULL) {
        return -1;
    }
    if (!net_packet_valid(pkt)) {
        atomic_fetch_add_explicit(&s->bad, 1, memory_order_relaxed);
        return 1;
    }
    atomic_fetch_add_explicit(&s->ok, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->bytes, (uint64_t)pkt->len, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->by_source[pkt->source_id], 1,
                              memory_order_relaxed);
    return 0;
}
