#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if !defined(__linux__)
#error "apps/rx_sink is Linux-only"
#endif

#include "net/capture.h"
#include "net/feed.h"
#include "net/race.h"
#include "net/sink.h"
#include "net/stats.h"
#include "net/time.h"
#include "net/util.h"
#include "shred/race.h"

#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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
            "usage: %s --bind IP --port N\n"
            "          [--out DIR] [--prefix NAME] [--rotate-bytes N]\n"
            "          [--min-gb N] [--ring N] [--cap-ring N] [--batch N]\n"
            "          [--cpu N] [--sink-cpu N] [--rec-cpu N]\n"
            "          [--rcvbuf N] [--busy] [--busy-us N] [--mlock]\n",
            prog);
}

struct sink_arg {
    net_race_t   *mux;
    shred_race_t *tab;
    net_sink_t   *sink;
};

static void
claim_one(struct sink_arg *a, const net_packet_t *pkt)
{
    shred_race_result_t r;

    if (shred_race_claim(a->tab, pkt, &r) == SHRED_RACE_WIN) {
        (void)net_sink_consume(a->sink, pkt);
    }
}

static void *
sink_thread(void *arg)
{
    struct sink_arg *a = arg;
    net_packet_t pkt;

    while (g_run) {
        if (net_race_acquire(a->mux, &pkt) == 0) {
            claim_one(a, &pkt);
            net_race_release(a->mux);
        } else {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000L };
            nanosleep(&ts, NULL);
        }
    }
    while (net_race_acquire(a->mux, &pkt) == 0) {
        claim_one(a, &pkt);
        net_race_release(a->mux);
    }
    return NULL;
}

static void
print_stats(const net_stats_t *st, const net_sink_t *sink,
            const shred_race_t *tab, const net_ring_t *ingress,
            const net_ring_t *cap)
{
    shred_race_stats_t rst;
    uint64_t first_tot = 0;
    uint8_t i, j;

    shred_race_stats(tab, &rst);
    for (i = 1; i < NET_SOURCE_MAX; i++) {
        first_tot += rst.first[i];
    }

    fprintf(stderr,
            "RX  pkt=%" PRIu64 " bytes=%" PRIu64 " bad=%" PRIu64
            " ring_drop=%" PRIu64 " cap_drop=%" PRIu64 " trunc=%" PRIu64
            " sink_ok=%" PRIu64 " shred_bad=%" PRIu64 " q=%" PRIu64,
            st->rx, st->bytes, st->malformed, st->ring_drops, st->cap_drops,
            st->trunc,
            atomic_load_explicit(&sink->ok, memory_order_relaxed),
            rst.bad, net_ring_depth(ingress));
    if (cap != NULL) {
        fprintf(stderr, " cap_q=%" PRIu64, net_ring_depth(cap));
    }
    fprintf(stderr, "\n");
    for (i = 1; i < NET_SOURCE_MAX; i++) {
        if (st->rx_by_source[i] == 0 && rst.first[i] == 0 && rst.late[i] == 0) {
            continue;
        }
        double pct = (first_tot == 0) ? 0.0
                                      : 100.0 * (double)rst.first[i] /
                                            (double)first_tot;
        fprintf(stderr,
                "     %s  rx=%" PRIu64 " first=%" PRIu64 " late=%" PRIu64
                "  first%%=%.1f\n",
                net_source_name(i), st->rx_by_source[i], rst.first[i],
                rst.late[i], pct);
    }
    for (i = 1; i < NET_SOURCE_MAX; i++) {
        for (j = 1; j < NET_SOURCE_MAX; j++) {
            uint64_t n;
            int64_t mn, p50, p95, p99, mx;
            if (rst.pair_n[i][j] == 0) {
                continue;
            }
            (void)shred_race_pair_pct(tab, i, j, &n, &mn, &p50, &p95, &p99,
                                      &mx);
            fprintf(stderr,
                    "     %s→%s  n=%" PRIu64 " min=%" PRId64 " p50=%" PRId64
                    " p95=%" PRId64 " p99=%" PRId64 " max=%" PRId64 " ns\n",
                    net_source_name(i), net_source_name(j), n, mn, p50, p95,
                    p99, mx);
        }
    }
}

int
main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0";
    const char *out_dir = NULL;
    const char *prefix = "orbitflare";
    uint16_t port = NET_ORBITFLARE_DEFAULT_PORT;
    uint64_t rotate = 2ull << 30;
    uint64_t min_gb = 20;
    uint64_t ring_n = 32768;
    uint64_t cap_n = 32768;
    uint64_t batch = 32;
    uint64_t rcvbuf = 16ull * 1024ull * 1024ull;
    int cpu = -1;
    int sink_cpu = -1;
    int rec_cpu = -1;
    int do_mlock = 0;
    int busy = 0;
    int busy_us = 50;
    int i;
    static net_feed_t feed;
    net_ring_t ingress;
    net_ring_t cap_ring;
    net_stats_t stats;
    net_sink_t sink;
    net_race_t mux;
    shred_race_t *tab = NULL;
    net_recorder_t *rec = NULL;
    netcap_hdr_t hdr;
    net_tsc_clock_t tsc;
    pthread_t sink_th;
    struct sink_arg sarg;
    uint64_t last_print;
    int have_cap = 0;

    memset(&ingress, 0, sizeof(ingress));
    memset(&cap_ring, 0, sizeof(cap_ring));
    net_stats_clear(&stats);
    net_sink_clear(&sink);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bind") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            bind_ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0) {
            uint64_t v;
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &v) != 0 || v == 0 || v > 65535) {
                return 1;
            }
            port = (uint16_t)v;
        } else if (strcmp(argv[i], "--out") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--rotate-bytes") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &rotate) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--min-gb") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &min_gb) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--ring") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &ring_n) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--cap-ring") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &cap_n) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--batch") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &batch) != 0 || batch == 0 ||
                batch > NET_RX_BATCH_MAX) {
                return 1;
            }
        } else if (strcmp(argv[i], "--cpu") == 0) {
            uint64_t v;
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &v) != 0) {
                return 1;
            }
            cpu = (int)v;
        } else if (strcmp(argv[i], "--sink-cpu") == 0) {
            uint64_t v;
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &v) != 0) {
                return 1;
            }
            sink_cpu = (int)v;
        } else if (strcmp(argv[i], "--rec-cpu") == 0) {
            uint64_t v;
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &v) != 0) {
                return 1;
            }
            rec_cpu = (int)v;
        } else if (strcmp(argv[i], "--rcvbuf") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &rcvbuf) != 0 || rcvbuf < 65536ull ||
                rcvbuf > (1ull << 30)) {
                return 1;
            }
        } else if (strcmp(argv[i], "--busy") == 0) {
            busy = 1;
        } else if (strcmp(argv[i], "--busy-us") == 0) {
            uint64_t v;
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &v) != 0 || v == 0 || v > 1000000) {
                return 1;
            }
            busy_us = (int)v;
        } else if (strcmp(argv[i], "--mlock") == 0) {
            do_mlock = 1;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (net_pin_cpu(cpu) != 0) {
        return 1;
    }
    if (net_tsc_calibrate(&tsc) != 0) {
        fprintf(stderr, "tsc calibrate failed\n");
        return 1;
    }
    if (net_ring_init(&ingress, (uint32_t)ring_n) != 0) {
        fprintf(stderr, "ingress ring\n");
        return 1;
    }
    if (out_dir != NULL) {
        uint64_t disk = 0;
        if (net_disk_free_bytes(out_dir, &disk) != 0) {
            net_ring_free(&ingress);
            return 1;
        }
        if (disk < min_gb * 1000000000ull) {
            fprintf(stderr, "disk: %" PRIu64 " bytes free, need %" PRIu64 " GB\n",
                    disk, min_gb);
            net_ring_free(&ingress);
            return 1;
        }
        if (net_ring_init(&cap_ring, (uint32_t)cap_n) != 0) {
            fprintf(stderr, "capture ring\n");
            net_ring_free(&ingress);
            return 1;
        }
        have_cap = 1;
    }
    if (net_orbitflare_open(&feed, bind_ip, port, &ingress,
                            have_cap ? &cap_ring : NULL, &stats,
                            (uint32_t)batch) != 0) {
        perror("orbitflare bind");
        if (have_cap) {
            net_ring_free(&cap_ring);
        }
        net_ring_free(&ingress);
        return 1;
    }
    {
        int got = net_feed_set_rcvbuf(&feed, (int)rcvbuf);
        if (got < 0) {
            perror("SO_RCVBUF");
            net_feed_close(&feed);
            if (have_cap) {
                net_ring_free(&cap_ring);
            }
            net_ring_free(&ingress);
            return 1;
        }
        fprintf(stderr, "RCVBUF  requested %d  kernel %d\n", (int)rcvbuf, got);
    }
    if (busy) {
        if (net_feed_set_busy_poll(&feed, busy_us) != 0) {
            fprintf(stderr, "SO_BUSY_POLL failed (userspace spin still on)\n");
        }
    }
    if (do_mlock && net_lock_memory() != 0) {
        net_feed_close(&feed);
        if (have_cap) {
            net_ring_free(&cap_ring);
        }
        net_ring_free(&ingress);
        return 1;
    }
    hdr.realtime0_ns = net_realtime_ns();
    hdr.mono0_ns = net_now_ns();
    hdr.tsc_hz = (uint64_t)tsc.hz;
    if (have_cap) {
        if (net_recorder_start(&rec, &cap_ring, out_dir, prefix, rotate,
                               rec_cpu, &hdr) != 0) {
            fprintf(stderr, "recorder\n");
            net_feed_close(&feed);
            net_ring_free(&cap_ring);
            net_ring_free(&ingress);
            return 1;
        }
    }

    if (shred_race_init(&tab, SHRED_RACE_DEFAULT_CAP) != 0) {
        fprintf(stderr, "shred race table\n");
        return 1;
    }
    net_race_init(&mux, &stats);
    if (net_race_add(&mux, &ingress) != 0) {
        shred_race_free(tab);
        return 1;
    }
    sarg.mux = &mux;
    sarg.tab = tab;
    sarg.sink = &sink;
    if (pthread_create(&sink_th, NULL, sink_thread, &sarg) != 0) {
        fprintf(stderr, "sink thread\n");
        if (rec != NULL) {
            net_recorder_stop(rec);
        }
        net_feed_close(&feed);
        if (have_cap) {
            net_ring_free(&cap_ring);
        }
        net_ring_free(&ingress);
        shred_race_free(tab);
        return 1;
    }
    if (sink_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(sink_cpu, &set);
        (void)pthread_setaffinity_np(sink_th, sizeof(set), &set);
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    last_print = net_now_ns();
    fprintf(stderr,
            "RX SINK  orbitflare %s:%u  batch=%" PRIu64 "  wait=%s  tsc_hz=%"
            PRIu64,
            bind_ip, port, batch, busy ? "busy" : "poll", hdr.tsc_hz);
    if (rec != NULL) {
        fprintf(stderr, "  capture %s", net_recorder_path(rec));
    }
    fprintf(stderr, "\n");

    while (g_run) {
        uint64_t now;
        if (net_feed_poll(&feed, busy ? -1 : 200) < 0) {
            perror("recv");
            break;
        }
        now = net_now_ns();
        if (now - last_print >= 1000000000ull) {
            net_stats_t snap;
            net_stats_snapshot(&stats, &snap);
            print_stats(&snap, &sink, tab, &ingress,
                        have_cap ? &cap_ring : NULL);
            last_print = now;
        }
    }

    pthread_join(sink_th, NULL);
    if (rec != NULL) {
        net_recorder_stop(rec);
    }
    net_feed_close(&feed);
    if (have_cap) {
        net_ring_free(&cap_ring);
    }
    net_ring_free(&ingress);
    shred_race_free(tab);
    return 0;
}
