#include "net/packet.h"
#include "net/race.h"
#include "net/ring.h"
#include "net/stats.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL  %s\n", (msg)); \
            g_fail++;                             \
        } else {                                  \
            printf("ok    %s\n", (msg));          \
        }                                         \
    } while (0)

static void
mk_pkt(net_packet_t *p, uint8_t *buf, uint8_t src, uint64_t rx_ns, uint32_t seq)
{
    memset(buf, (int)src, 64);
    p->data = buf;
    p->len = 64;
    p->source_id = src;
    p->flags = 0;
    p->seq = seq;
    p->rx_ns = rx_ns;
    p->rx_tsc = rx_ns * 3u;
}

int
main(void)
{
    net_ring_t of, dz;
    net_race_t mux;
    net_stats_t stats;
    net_packet_t pkt, a;
    uint8_t buf_a[64], buf_b[64];

    net_stats_clear(&stats);
    CHECK(net_ring_init(&of, 8) == 0, "orbitflare ring");
    CHECK(net_ring_init(&dz, 8) == 0, "doublezero ring");
    net_race_init(&mux, &stats);
    CHECK(net_race_add(&mux, &of) == 0, "add feed 1");
    CHECK(net_race_add(&mux, &dz) == 0, "add feed 2 without touching OF RX");

    mk_pkt(&pkt, buf_b, NET_SOURCE_DOUBLEZERO, 100, 1);
    CHECK(net_ring_push(&dz, &pkt) == 0, "only DZ ready");
    CHECK(net_race_acquire(&mux, &a) == 0 &&
              a.source_id == NET_SOURCE_DOUBLEZERO,
          "do not wait for OF; claim on arrival");
    net_race_release(&mux);

    mk_pkt(&pkt, buf_a, NET_SOURCE_ORBITFLARE, 5000, 2);
    CHECK(net_ring_push(&of, &pkt) == 0, "OF later timestamp");
    mk_pkt(&pkt, buf_b, NET_SOURCE_DOUBLEZERO, 1000, 2);
    CHECK(net_ring_push(&dz, &pkt) == 0, "DZ earlier timestamp");
    CHECK(net_race_acquire(&mux, &a) == 0 &&
              a.source_id == NET_SOURCE_ORBITFLARE,
          "RR / first ready ring, not min(rx_ns)");
    net_race_release(&mux);
    CHECK(net_race_acquire(&mux, &a) == 0 &&
              a.source_id == NET_SOURCE_DOUBLEZERO,
          "other ring still claimed immediately");
    net_race_release(&mux);
    CHECK(net_race_acquire(&mux, &a) == 1, "empty");

    net_ring_free(&of);
    net_ring_free(&dz);

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nnet_race ok\n");
    return 0;
}
