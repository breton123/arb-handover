#include "feed.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void
usage(const char *prog)
{
    fprintf(stderr, "usage: %s --file capture.cap\n", prog);
}

int
main(int argc, char **argv)
{
    const char *path = NULL;
    FILE *f;
    feedcap_hdr_t hdr;
    feed_slot_t slot;
    feed_stats_t stats;
    rx_packet_t pkt;
    int i;

    memset(&stats, 0, sizeof(stats));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (path == NULL) {
        usage(argv[0]);
        return 1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    if (feedcap_read_header(f, &hdr) != 0) {
        fprintf(stderr, "bad header\n");
        fclose(f);
        return 1;
    }
    for (;;) {
        int rc = feedcap_read_slot(f, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fprintf(stderr, "bad record\n");
            fclose(f);
            return 1;
        }
        pkt.data  = slot.data;
        pkt.len   = (uint16_t)slot.len;
        pkt.queue = 0;
        feed_observe(&stats, &pkt);
    }
    fclose(f);
    printf("REPLAY  %s\n", path);
    printf("  realtime0  %" PRIu64 "\n", hdr.realtime0_ns);
    printf("  mono0      %" PRIu64 "\n", hdr.mono0_ns);
    printf("  tsc_hz     %" PRIu64 "\n", hdr.tsc_hz);
    printf("  rx         %" PRIu64 "\n", atomic_load(&stats.rx));
    printf("  bytes      %" PRIu64 "\n", atomic_load(&stats.bytes));
    printf("  bad        %" PRIu64 "\n", atomic_load(&stats.bad));
    printf("  shred      %" PRIu64 "\n", atomic_load(&stats.shred_ok));
    printf("  data       %" PRIu64 "\n", atomic_load(&stats.data));
    printf("  coding     %" PRIu64 "\n", atomic_load(&stats.coding));
    printf("  relevant   %" PRIu64 "\n", atomic_load(&stats.relevant));
    printf("  dlmm       %" PRIu64 "\n", atomic_load(&stats.dlmm));
    printf("  pump       %" PRIu64 "\n", atomic_load(&stats.pump));
    return 0;
}
