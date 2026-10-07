#include "net/race.h"

#include <string.h>

void
net_race_init(net_race_t *r, net_stats_t *stats)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->stats = stats;
}

int
net_race_add(net_race_t *r, net_ring_t *ring)
{
    if (r == NULL || ring == NULL || r->n >= NET_SOURCE_MAX) {
        return -1;
    }
    r->ring[r->n] = ring;
    r->n++;
    return 0;
}

int
net_race_acquire(net_race_t *r, net_packet_t *out)
{
    uint8_t k, i;
    int rc;

    if (r == NULL || out == NULL || r->held != 0u || r->n == 0u) {
        return -1;
    }
    for (k = 0; k < r->n; k++) {
        i = (uint8_t)((r->next + k) % r->n);
        rc = net_ring_peek(r->ring[i], out);
        if (rc != 0) {
            continue;
        }
        rc = net_ring_acquire(r->ring[i], out);
        if (rc != 0) {
            return rc;
        }
        r->held = (uint8_t)(i + 1u);
        r->next = (uint8_t)((i + 1u) % r->n);
        return 0;
    }
    return 1;
}

void
net_race_release(net_race_t *r)
{
    uint8_t idx;

    if (r == NULL || r->held == 0u) {
        return;
    }
    idx = (uint8_t)(r->held - 1u);
    net_ring_release(r->ring[idx]);
    r->held = 0;
}
