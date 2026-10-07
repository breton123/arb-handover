#include "feed.h"
#include "tsc.h"
#include "util.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_run = 1;

static void
on_sig(int sig)
{
    (void)sig;
    g_run = 0;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --bind IP --port N --out DIR\n"
            "          [--prefix NAME] [--rotate-bytes N] [--min-gb N]\n"
            "          [--cpu N] [--rec-cpu N] [--mlock] [--ring N]\n"
            "          [--rcvbuf N]\n",
            prog);
}

int
main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0";
    const char *out_dir = NULL;
    const char *prefix = "feed";
    uint16_t port = 0;
    uint64_t rotate = 2ull << 30;
    uint64_t min_gb = 20;
    uint64_t ring_n = 32768;
    int cpu = -1;
    int rec_cpu = -1;
    int do_mlock = 0;
    int rcvbuf = 16 * 1024 * 1024;
    udp_source_t src;
    feed_ring_t ring;
    feed_stats_t stats;
    recorder_t *rec = NULL;
    feedcap_hdr_t hdr;
    struct tsc_clock tsc;
    uint8_t buf[65507];
    rx_packet_t pkt;
    uint32_t seq = 0;
    uint64_t disk = 0;
    uint64_t last_print = 0;
    int i;

    src.fd = -1;
    memset(&stats, 0, sizeof(stats));
    memset(&ring, 0, sizeof(ring));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            bind_ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0) {
            uint64_t v;
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (parse_u64(argv[++i], &v) != 0 || v == 0 || v > 65535) {
                return 1;
            }
            port = (uint16_t)v;
        } else if (strcmp(argv[i], "--out") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--rotate-bytes") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (parse_u64(argv[++i], &rotate) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--min-gb") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (parse_u64(argv[++i], &min_gb) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--ring") == 0) {
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (parse_u64(argv[++i], &ring_n) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--cpu") == 0) {
            uint64_t v;
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (parse_u64(argv[++i], &v) != 0) {
                return 1;
            }
            cpu = (int)v;
        } else if (strcmp(argv[i], "--rec-cpu") == 0) {
            uint64_t v;
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (parse_u64(argv[++i], &v) != 0) {
                return 1;
            }
            rec_cpu = (int)v;
        } else if (strcmp(argv[i], "--mlock") == 0) {
            do_mlock = 1;
        } else if (strcmp(argv[i], "--rcvbuf") == 0) {
            uint64_t v;
            if (require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (parse_u64(argv[++i], &v) != 0 || v < 65536ull || v > (1ull << 30)) {
                return 1;
            }
            rcvbuf = (int)v;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (port == 0 || out_dir == NULL) {
        usage(argv[0]);
        return 1;
    }

    if (pin_cpu(cpu) != 0) {
        return 1;
    }
    if (tsc_calibrate(&tsc) != 0) {
        return 1;
    }
    if (disk_free_bytes(out_dir, &disk) != 0) {
        return 1;
    }
    if (disk < min_gb * 1000000000ull) {
        fprintf(stderr, "disk: %" PRIu64 " bytes free, need %" PRIu64 " GB\n",
                disk, min_gb);
        return 1;
    }
    hdr.realtime0_ns = now_realtime_ns();
    hdr.mono0_ns     = now_ns();
    hdr.tsc_hz       = (uint64_t)tsc.hz;
    fprintf(stderr,
            "PREFLIGHT  bind %s:%u  disk %" PRIu64 " GB  realtime %" PRIu64
            "  mono %" PRIu64 "  tsc_hz %" PRIu64 "\n",
            bind_ip, port, (uint64_t)(disk / 1000000000ull),
            hdr.realtime0_ns, hdr.mono0_ns, hdr.tsc_hz);

    if (udp_source_open(&src, bind_ip, port) != 0) {
        perror("bind");
        return 1;
    }
    {
        int got = udp_source_set_rcvbuf(&src, rcvbuf);
        if (got < 0) {
            perror("SO_RCVBUF");
            return 1;
        }
        fprintf(stderr, "RCVBUF  requested %d  kernel %d\n", rcvbuf, got);
    }
    if (feed_ring_init(&ring, (uint32_t)ring_n) != 0) {
        fprintf(stderr, "ring\n");
        return 1;
    }
    if (do_mlock && lock_memory() != 0) {
        return 1;
    }
    if (recorder_start(&rec, &ring, out_dir, prefix, rotate, rec_cpu, &hdr) != 0) {
        fprintf(stderr, "recorder\n");
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    last_print = now_ns();
    fprintf(stderr, "LIVE  capture %s  leave it alone\n", recorder_path(rec));

    while (g_run) {
        uint64_t tsc0, ns0;
        if (udp_source_recv(&src, buf, sizeof(buf), &pkt) != 0) {
            continue;
        }
        ns0  = now_ns();
        tsc0 = rdtscp();
        (void)feed_try_push(&ring, &pkt, ns0, tsc0, seq++);
        feed_observe(&stats, &pkt);
        if (ns0 - last_print >= 1000000000ull) {
            feed_stats_t s;
            feed_stats_snapshot(&stats, &s);
            fprintf(stderr,
                    "FEED LIVE  rx=%" PRIu64 " bytes=%" PRIu64 " bad=%" PRIu64
                    " shred=%" PRIu64 " data=%" PRIu64 " coding=%" PRIu64
                    " rel=%" PRIu64 " dlmm=%" PRIu64 " pump=%" PRIu64
                    " cap_drop=%" PRIu64 "\n",
                    s.rx, s.bytes, s.bad, s.shred_ok, s.data, s.coding,
                    s.relevant, s.dlmm, s.pump,
                    atomic_load(&ring.drops));
            last_print = ns0;
        }
    }

    recorder_stop(rec);
    feed_ring_free(&ring);
    udp_source_close(&src);
    return 0;
}
