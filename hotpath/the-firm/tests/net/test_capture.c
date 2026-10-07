#include "net/capture.h"
#include "net/packet.h"
#include "net/ring.h"
#include "net/time.h"

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

#if defined(__linux__)
#define CAP_PATH "/tmp/firm_net_test.cap"
#else
#define CAP_PATH "firm_net_test.cap"
#endif

int
main(void)
{
    netcap_hdr_t hdr, hdr2;
    net_slot_t a, b;
    net_ring_t ring;
    net_packet_t pkt;
    net_recorder_t *rec = NULL;
    uint8_t wire[200];
    FILE *f;
    uint32_t i;

    memset(wire, 0x22, sizeof(wire));
    memset(&a, 0, sizeof(a));
    a.rx_ns = 111;
    a.rx_tsc = 222;
    a.len = 200;
    a.seq = 7;
    a.source_id = NET_SOURCE_ORBITFLARE;
    a.flags = 0;
    memcpy(a.data, wire, 200);

    hdr.realtime0_ns = 100;
    hdr.mono0_ns = 200;
    hdr.tsc_hz = 300;

    f = fopen(CAP_PATH, "wb");
    CHECK(f != NULL, "open cap");
    CHECK(netcap_write_header(f, &hdr) == 0 && netcap_write_slot(f, &a) == 0,
          "write cap");
    fclose(f);

    f = fopen(CAP_PATH, "rb");
    CHECK(f != NULL, "reopen cap");
    CHECK(netcap_read_header(f, &hdr2) == 0 && hdr2.tsc_hz == 300, "read header");
    CHECK(netcap_read_slot(f, &b) == 0 && b.seq == 7 && b.len == 200,
          "read slot");
    CHECK(b.source_id == NET_SOURCE_ORBITFLARE, "source_id on disk");
    CHECK(memcmp(b.data, wire, 200) == 0, "raw datagram preserved");
    fclose(f);

    pkt.data = wire;
    pkt.len = 200;
    pkt.source_id = NET_SOURCE_ORBITFLARE;
    pkt.flags = 0;
    pkt.seq = 0;
    pkt.rx_ns = 1;
    pkt.rx_tsc = 2;
    CHECK(net_ring_init(&ring, 64) == 0, "cap ring");
    hdr.realtime0_ns = net_realtime_ns();
    hdr.mono0_ns = net_now_ns();
    hdr.tsc_hz = 1;
    CHECK(net_recorder_start(&rec, &ring, ".", "firmcap", 1ull << 30, -1,
                             &hdr) == 0,
          "recorder start");
    {
        char path[640];
        snprintf(path, sizeof(path), "%s", net_recorder_path(rec));
        for (i = 0; i < 32; i++) {
            pkt.seq = i;
            pkt.rx_ns = 10 + i;
            if (net_ring_push(&ring, &pkt) != 0) {
                CHECK(0, "cap push");
                break;
            }
        }
        net_recorder_stop(rec);
        rec = NULL;
        f = fopen(path, "rb");
        CHECK(f != NULL, "open recorded file");
        CHECK(netcap_read_header(f, &hdr2) == 0, "recorded header");
        i = 0;
        while (netcap_read_slot(f, &b) == 0) {
            if (b.source_id != NET_SOURCE_ORBITFLARE) {
                CHECK(0, "recorded source");
                break;
            }
            i++;
        }
        fclose(f);
        CHECK(i == 32, "recorder drained 32");
        (void)remove(path);
    }
    net_ring_free(&ring);
    (void)remove(CAP_PATH);

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nnet_capture ok\n");
    return 0;
}
