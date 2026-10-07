#include "net/packet.h"
#include "net/ring.h"

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

int
main(void)
{
    net_ring_t ring;
    net_packet_t pkt;
    net_packet_t a, b;
    net_slot_t *claimed[4];
    uint8_t wire[200];
    uint32_t n;

    memset(wire, 0x11, sizeof(wire));
    pkt.data = wire;
    pkt.len = 200;
    pkt.source_id = NET_SOURCE_ORBITFLARE;
    pkt.flags = 0;
    pkt.seq = 10;
    pkt.rx_ns = 100;
    pkt.rx_tsc = 200;

    CHECK(net_source_valid(NET_SOURCE_ORBITFLARE), "orbitflare id reserved");
    CHECK(net_source_valid(NET_SOURCE_DOUBLEZERO), "doublezero id reserved");
    CHECK(net_source_valid(NET_SOURCE_BLOCKSPACE), "blockspace id reserved");
    CHECK(!net_source_valid(NET_SOURCE_INVALID), "id 0 invalid");
    CHECK(NET_SOURCE_DOUBLEZERO != NET_SOURCE_ORBITFLARE,
          "feed #2 is a distinct id");

    CHECK(net_ring_init(&ring, 3) != 0, "reject non-power-of-two");
    CHECK(net_ring_init(&ring, 4) == 0, "ring init");
    CHECK(net_ring_capacity(&ring) == 4, "capacity");
    CHECK(net_ring_push(&ring, &pkt) == 0, "push 0");
    pkt.seq = 11;
    pkt.rx_ns = 101;
    CHECK(net_ring_push(&ring, &pkt) == 0, "push 1");
    pkt.seq = 12;
    CHECK(net_ring_push(&ring, &pkt) == 0, "push 2");
    pkt.seq = 13;
    CHECK(net_ring_push(&ring, &pkt) == 0, "push 3");
    pkt.seq = 14;
    CHECK(net_ring_push(&ring, &pkt) == 1, "full ring drops");
    CHECK(atomic_load(&ring.drops) == 1, "drop counted");

    CHECK(net_ring_peek(&ring, &a) == 0 && a.seq == 10 && a.rx_ns == 100,
          "peek first");
    CHECK(net_ring_acquire(&ring, &b) == 0 && b.seq == 10, "acquire first");
    CHECK(b.len == 200 && memcmp(b.data, wire, 200) == 0, "payload alias");
    CHECK(b.source_id == NET_SOURCE_ORBITFLARE, "source preserved");
    net_ring_release(&ring);
    CHECK(net_ring_acquire(&ring, &a) == 0 && a.seq == 11, "acquire next");
    net_ring_release(&ring);

    CHECK(net_ring_push(&ring, &pkt) == 0, "push after release");
    net_ring_free(&ring);

    CHECK(net_ring_init(&ring, 8) == 0, "claim ring");
    n = net_ring_claim(&ring, claimed, 3);
    CHECK(n == 3, "claim 3");
    claimed[0]->source_id = NET_SOURCE_ORBITFLARE;
    claimed[0]->len = 4;
    claimed[0]->seq = 1;
    claimed[0]->rx_ns = 7;
    memcpy(claimed[0]->data, "abcd", 4);
    claimed[1]->source_id = NET_SOURCE_DOUBLEZERO;
    claimed[1]->len = 4;
    claimed[1]->seq = 2;
    claimed[1]->rx_ns = 8;
    memcpy(claimed[1]->data, "efgh", 4);
    claimed[2]->source_id = NET_SOURCE_BLOCKSPACE;
    claimed[2]->len = 4;
    claimed[2]->seq = 3;
    claimed[2]->rx_ns = 9;
    memcpy(claimed[2]->data, "ijkl", 4);
    net_ring_commit(&ring, 2);
    CHECK(net_ring_depth(&ring) == 2, "commit 2 of 3");
    CHECK(net_ring_acquire(&ring, &a) == 0 && a.seq == 1 &&
              a.source_id == NET_SOURCE_ORBITFLARE,
          "first claimed slot");
    net_ring_release(&ring);
    CHECK(net_ring_acquire(&ring, &a) == 0 && a.seq == 2 &&
              a.source_id == NET_SOURCE_DOUBLEZERO,
          "second source is not orbitflare");
    net_ring_release(&ring);
    CHECK(net_ring_acquire(&ring, &a) == 1, "unpublished slot not visible");
    net_ring_free(&ring);

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nnet_ring ok\n");
    return 0;
}
