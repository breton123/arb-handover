#if !defined(__linux__)
#error "tests/net/test_rx.c is Linux-only"
#endif

#include "net/feed.h"
#include "net/race.h"
#include "net/sink.h"
#include "net/stats.h"
#include "shred/race.h"
#include "shred/shred.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

#define TEST_PORT 39911

static void
fill_shred(uint8_t *p, uint16_t n, uint64_t slot, uint32_t index)
{
    uint16_t ver = 1;
    uint32_t fec = 1;
    memset(p, 0, n);
    p[SHRED_OFF_VARIANT] = SHRED_TYPE_CHAINED_DATA;
    memcpy(p + SHRED_OFF_SLOT, &slot, 8);
    memcpy(p + SHRED_OFF_INDEX, &index, 4);
    memcpy(p + SHRED_OFF_VERSION, &ver, 2);
    memcpy(p + SHRED_OFF_FEC, &fec, 4);
}

int
main(void)
{
    static net_feed_t feed;
    net_ring_t ring;
    net_stats_t stats;
    net_sink_t sink;
    net_race_t mux;
    shred_race_t *tab = NULL;
    shred_race_result_t claim;
    shred_race_stats_t rst;
    net_packet_t pkt;
    struct sockaddr_in dest;
    int snd;
    uint8_t wire[200];
    int i, got;

    net_stats_clear(&stats);
    net_sink_clear(&sink);
    CHECK(shred_race_init(&tab, 1024) == 0, "race table");
    CHECK(net_ring_init(&ring, 64) == 0, "ring");
    CHECK(net_orbitflare_open(&feed, "127.0.0.1", TEST_PORT, &ring, NULL,
                              &stats, 8) == 0,
          "orbitflare bind");

    snd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(snd >= 0, "sender");
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    for (i = 0; i < 3; i++) {
        fill_shred(wire, 200, 100, (uint32_t)i);
        CHECK(sendto(snd, wire, 200, 0, (struct sockaddr *)&dest,
                     sizeof(dest)) == 200,
              "send datagram");
    }
    close(snd);

    got = 0;
    for (i = 0; i < 20 && got < 3; i++) {
        int n = net_feed_poll(&feed, 200);
        CHECK(n >= 0, "poll");
        if (n > 0) {
            got += n;
        }
    }
    CHECK(got == 3, "three packets into ring");

    net_race_init(&mux, &stats);
    CHECK(net_race_add(&mux, &ring) == 0, "mux add");
    for (i = 0; i < 3; i++) {
        CHECK(net_race_acquire(&mux, &pkt) == 0, "sink acquire");
        CHECK(pkt.source_id == NET_SOURCE_ORBITFLARE, "source_id stamped");
        CHECK(pkt.len == 200 && pkt.rx_ns != 0, "payload + timestamp");
        CHECK(net_packet_valid(&pkt), "contract");
        CHECK(shred_race_claim(tab, &pkt, &claim) == SHRED_RACE_WIN, "unique win");
        CHECK(claim.id.index == (uint32_t)i, "index");
        CHECK(net_sink_consume(&sink, &pkt) == 0, "sink winners only");
        net_race_release(&mux);
    }
    CHECK(atomic_load(&sink.ok) == 3 && atomic_load(&sink.bad) == 0,
          "sink counts");
    CHECK(atomic_load(&stats.rx) == 3, "rx counter");
    shred_race_stats(tab, &rst);
    CHECK(rst.first[NET_SOURCE_ORBITFLARE] == 3, "single feed all first");
    CHECK(rst.late[NET_SOURCE_ORBITFLARE] == 0, "no dups");

    net_feed_close(&feed);
    net_ring_free(&ring);
    shred_race_free(tab);

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nnet_rx ok\n");
    return 0;
}
