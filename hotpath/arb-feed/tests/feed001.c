#include "feed.h"
#include "tsc.h"

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

static void
mk_shred(uint8_t *p, uint16_t n)
{
    memset(p, 0, n);
    p[64] = (uint8_t)SHRED_TYPE_CHAINED_DATA;
    p[65] = 1;
}

int
main(void)
{
    udp_source_t src;
    struct sockaddr_in dest;
    int snd;
    uint8_t buf[2048], wire[200];
    rx_packet_t pkt;
    feed_ring_t ring;
    feed_slot_t a, b;
    feedcap_hdr_t hdr;
    FILE *f;
    shred_view_t view;

    CHECK(sizeof(rx_packet_t) >= 2 * sizeof(void *), "rx_packet_t is the frozen layout");

    src.fd = -1;
    CHECK(udp_source_open(&src, "127.0.0.1", 39901) == 0, "UDP bind configurable");
    snd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(snd >= 0, "sender");
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(39901);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    mk_shred(wire, 200);
    CHECK(sendto(snd, wire, 200, 0, (struct sockaddr *)&dest, sizeof(dest)) == 200,
          "send datagram");
    CHECK(udp_source_recv(&src, buf, sizeof(buf), &pkt) == 0, "recv → rx_packet_t");
    CHECK(pkt.len == 200 && pkt.queue == 0 && memcmp(pkt.data, wire, 200) == 0,
          "raw packet → rx_packet_t");
    CHECK(hot_rx(&pkt, &view) == 0 && shred_is_data(view.type),
          "same boundary parses a shred");
    close(snd);
    udp_source_close(&src);

    CHECK(feed_ring_init(&ring, 4) == 0, "ring");
    pkt.data = wire;
    pkt.len  = 200;
    CHECK(feed_try_push(&ring, &pkt, 1, 2, 10) == 0, "push 0");
    CHECK(feed_try_push(&ring, &pkt, 3, 4, 11) == 0, "push 1");
    CHECK(feed_try_push(&ring, &pkt, 5, 6, 12) == 0, "push 2");
    CHECK(feed_try_push(&ring, &pkt, 7, 8, 13) == 0, "push 3");
    CHECK(feed_try_push(&ring, &pkt, 9, 10, 14) == 1, "full ring drops");
    CHECK(atomic_load(&ring.drops) == 1, "drop counted");
    CHECK(feed_pop(&ring, &a) == 0 && a.seq == 10 && a.rx_ns == 1, "pop");
    CHECK(feed_try_push(&ring, &pkt, 11, 12, 15) == 0, "push after pop");
    feed_ring_free(&ring);

    hdr.realtime0_ns = 100;
    hdr.mono0_ns     = 200;
    hdr.tsc_hz       = 300;
    memset(&a, 0, sizeof(a));
    a.rx_ns  = 111;
    a.rx_tsc = 222;
    a.len    = 200;
    a.seq    = 7;
    memcpy(a.data, wire, 200);
    f = fopen("/tmp/feed001.cap", "wb");
    CHECK(f != NULL, "open cap");
    CHECK(feedcap_write_header(f, &hdr) == 0 && feedcap_write_slot(f, &a) == 0,
          "write cap");
    fclose(f);
    f = fopen("/tmp/feed001.cap", "rb");
    CHECK(feedcap_read_header(f, &hdr) == 0 && hdr.tsc_hz == 300, "read header");
    CHECK(feedcap_read_slot(f, &b) == 0 && b.seq == 7 && b.len == 200, "read slot");
    CHECK(memcmp(b.data, wire, 200) == 0, "raw datagram preserved");
    fclose(f);
    pkt.data  = b.data;
    pkt.len   = (uint16_t)b.len;
    pkt.queue = 0;
    CHECK(hot_rx(&pkt, &view) == 0, "replay through same rx_packet_t");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nFEED-001 ok\n");
    return 0;
}
