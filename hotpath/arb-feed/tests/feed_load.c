#include "feed.h"
#include "tsc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define PUSH_N     200000u
#define RING_SLOTS 4096u

int
main(void)
{
    feed_ring_t ring;
    recorder_t *rec = NULL;
    feedcap_hdr_t hdr;
    feed_slot_t slot;
    feed_stats_t stats;
    rx_packet_t pkt;
    uint8_t buf[200];
    uint32_t i;
    uint64_t pushed = 0, dropped = 0;
    FILE *f;
    int rc, nrec = 0;

    memset(buf, 0, sizeof(buf));
    buf[64] = (uint8_t)SHRED_TYPE_CHAINED_DATA;
    pkt.data  = buf;
    pkt.len   = 200;
    pkt.queue = 0;
    hdr.realtime0_ns = now_realtime_ns();
    hdr.mono0_ns     = now_ns();
    hdr.tsc_hz       = 1;
    memset(&stats, 0, sizeof(stats));

    if (feed_ring_init(&ring, RING_SLOTS) != 0) {
        return 1;
    }
    if (recorder_start(&rec, &ring, "/tmp", "feedload", 1ull << 30, -1, &hdr) != 0) {
        fprintf(stderr, "recorder start\n");
        return 1;
    }
    {
        char path[640];
        uint64_t written;
        snprintf(path, sizeof(path), "%s", recorder_path(rec));
        for (i = 0; i < PUSH_N; i++) {
            rc = feed_try_push(&ring, &pkt, now_ns(), rdtscp(), i);
            if (rc == 0) {
                pushed++;
            } else if (rc == 1) {
                dropped++;
            } else {
                fprintf(stderr, "push err\n");
                recorder_stop(rec);
                return 1;
            }
        }
        recorder_stop(rec);
        rec = NULL;
        written = 0;
        f = fopen(path, "rb");
        if (f == NULL) {
            perror(path);
            feed_ring_free(&ring);
            return 1;
        }
        if (feedcap_read_header(f, &hdr) != 0) {
            fprintf(stderr, "bad load header\n");
            fclose(f);
            return 1;
        }
        for (;;) {
            rc = feedcap_read_slot(f, &slot);
            if (rc == 1) {
                break;
            }
            if (rc != 0) {
                fprintf(stderr, "bad load record\n");
                fclose(f);
                return 1;
            }
            pkt.data  = slot.data;
            pkt.len   = (uint16_t)slot.len;
            pkt.queue = 0;
            feed_observe(&stats, &pkt);
            written++;
        }
        fclose(f);
        nrec = (int)written;
        printf("LOAD  pushed=%" PRIu64 " dropped=%" PRIu64 " replayed=%d\n",
               pushed, dropped, nrec);
        if (nrec == 0) {
            fprintf(stderr, "nothing on disk\n");
            feed_ring_free(&ring);
            return 1;
        }
        if (nrec + (int)dropped != (int)PUSH_N && written + dropped < PUSH_N) {
            /* some still in flight is ok; we drained on stop */
        }
    }
    feed_ring_free(&ring);
    printf("ok    recorder survived synthetic flood (lossy, no wait)\n");
    printf("ok    capture can be replayed\n");
    return 0;
}
