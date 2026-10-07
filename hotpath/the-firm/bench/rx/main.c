#include "net/packet.h"
#include "net/ring.h"
#include "net/time.h"
#include "net/util.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include "net/feed.h"
#include "net/race.h"
#include "net/sink.h"
#include "net/stats.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define DEFAULT_COUNT 200000u
#define DEFAULT_SLOTS 4096u
#define DEFAULT_SIZE  1200u
#define DEFAULT_BATCH 32u

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

static uint64_t
percentile(uint64_t *v, size_t n, double p)
{
    size_t i;

    if (n == 0) {
        return 0;
    }
    i = (size_t)(p * (double)(n - 1));
    return v[i];
}

static void
print_lat(const char *name, uint64_t ns)
{
    printf("  %-8s %10" PRIu64 " ns  (%.3f us)\n", name, ns,
           (double)ns / 1000.0);
}

static void
report(const char *title, uint64_t *samples, size_t n, const net_tsc_clock_t *tsc)
{
    size_t i;
    uint64_t *ns;
    uint64_t sum = 0;

    printf("%s\n", title);
    if (n == 0) {
        printf("  (no samples)\n");
        return;
    }
    ns = calloc(n, sizeof(*ns));
    if (ns == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        ns[i] = net_tsc_to_ns(tsc, samples[i]);
        sum += ns[i];
    }
    qsort(ns, n, sizeof(*ns), cmp_u64);
    print_lat("min", ns[0]);
    print_lat("p50", percentile(ns, n, 0.50));
    print_lat("p95", percentile(ns, n, 0.95));
    print_lat("p99", percentile(ns, n, 0.99));
    print_lat("max", ns[n - 1]);
    printf("  %-8s %10.1f ns\n", "mean", (double)sum / (double)n);
    printf("  samples  %zu\n", n);
    free(ns);
}

static int
bench_ring(uint32_t count, uint32_t slots, uint16_t size, uint32_t warmup)
{
    net_ring_t ring;
    net_tsc_clock_t tsc;
    net_packet_t pkt;
    net_packet_t out;
    uint8_t payload[NET_PKT_MAX];
    uint64_t *push_cyc;
    uint64_t *pop_cyc;
    uint32_t i, n;
    uint64_t t0, t1;

    if (size == 0u || size > NET_PKT_MAX) {
        return 1;
    }
    if (net_tsc_calibrate(&tsc) != 0) {
        fprintf(stderr, "tsc calibrate failed\n");
        return 1;
    }
    if (net_ring_init(&ring, slots) != 0) {
        fprintf(stderr, "ring init\n");
        return 1;
    }
    memset(payload, 0x5a, size);
    pkt.data = payload;
    pkt.len = size;
    pkt.source_id = NET_SOURCE_ORBITFLARE;
    pkt.flags = 0;
    pkt.seq = 0;
    pkt.rx_ns = 1;
    pkt.rx_tsc = 2;

    push_cyc = calloc(count, sizeof(*push_cyc));
    pop_cyc = calloc(count, sizeof(*pop_cyc));
    if (push_cyc == NULL || pop_cyc == NULL) {
        net_ring_free(&ring);
        free(push_cyc);
        free(pop_cyc);
        return 1;
    }

    n = 0;
    for (i = 0; i < count + warmup; i++) {
        int rc;
        pkt.seq = i;
        pkt.rx_ns = (uint64_t)i + 1u;
        t0 = net_rdtscp();
        rc = net_ring_push(&ring, &pkt);
        t1 = net_rdtscp();
        if (rc != 0) {
            fprintf(stderr, "push failed (ring too small for in-flight?)\n");
            break;
        }
        if (i >= warmup && n < count) {
            push_cyc[n] = t1 - t0;
        }
        t0 = net_rdtscp();
        rc = net_ring_acquire(&ring, &out);
        if (rc == 0) {
            net_ring_release(&ring);
        }
        t1 = net_rdtscp();
        if (i >= warmup && n < count) {
            pop_cyc[n] = t1 - t0;
            n++;
        }
    }

    printf("RX BENCH  ring\n");
    printf("count=%u  slots=%u  size=%u  tsc_hz=%.6e\n\n", count, slots, size,
           tsc.hz);
    report("push (memcpy into slot + SPSC publish)", push_cyc, n, &tsc);
    printf("\n");
    report("acquire + release", pop_cyc, n, &tsc);
    printf("\nlatency is intra-process rdtscp, not NIC RX.\n");

    free(push_cyc);
    free(pop_cyc);
    net_ring_free(&ring);
    return 0;
}

#if defined(__linux__)

struct udp_send_arg {
    const char *ip;
    uint16_t port;
    uint32_t count;
    uint16_t size;
    uint32_t batch;
    volatile int *ready;
};

static void *
udp_sender(void *arg)
{
    struct udp_send_arg *a = arg;
    struct sockaddr_in dest;
    int fd;
    uint8_t *buf;
    uint32_t i;

    while (!*a->ready) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000L };
        nanosleep(&ts, NULL);
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return NULL;
    }
    buf = calloc(1, a->size);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    buf[0] = 0xA5;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(a->port);
    inet_pton(AF_INET, a->ip, &dest.sin_addr);
    for (i = 0; i < a->count; i++) {
        (void)sendto(fd, buf, a->size, 0, (struct sockaddr *)&dest,
                     sizeof(dest));
    }
    free(buf);
    close(fd);
    return NULL;
}

static int
bench_udp(uint32_t count, uint32_t slots, uint16_t size, uint32_t batch,
          uint16_t port)
{
    static net_feed_t feed;
    net_ring_t ring;
    net_stats_t stats;
    net_sink_t sink;
    net_race_t race;
    net_tsc_clock_t tsc;
    net_packet_t pkt;
    pthread_t th;
    struct udp_send_arg sarg;
    volatile int ready = 0;
    uint64_t *rx_cyc;
    uint64_t t0, t1, first_ns = 0, last_ns = 0;
    uint32_t got = 0;
    uint32_t n_cyc = 0;
    uint64_t idle_ns;

    if (net_tsc_calibrate(&tsc) != 0) {
        return 1;
    }
    if (net_ring_init(&ring, slots) != 0) {
        return 1;
    }
    net_stats_clear(&stats);
    net_sink_clear(&sink);
    if (net_orbitflare_open(&feed, "127.0.0.1", port, &ring, NULL, &stats,
                            batch) != 0) {
        perror("bind");
        net_ring_free(&ring);
        return 1;
    }
    (void)net_feed_set_rcvbuf(&feed, 16 * 1024 * 1024);
    net_race_init(&race, &stats);
    (void)net_race_add(&race, &ring);

    rx_cyc = calloc(count, sizeof(*rx_cyc));
    if (rx_cyc == NULL) {
        net_feed_close(&feed);
        net_ring_free(&ring);
        return 1;
    }

    sarg.ip = "127.0.0.1";
    sarg.port = port;
    sarg.count = count;
    sarg.size = size;
    sarg.batch = batch;
    sarg.ready = &ready;
    if (pthread_create(&th, NULL, udp_sender, &sarg) != 0) {
        free(rx_cyc);
        net_feed_close(&feed);
        net_ring_free(&ring);
        return 1;
    }
    ready = 1;
    idle_ns = net_now_ns();
    while (got < count) {
        int n;
        t0 = net_rdtscp();
        n = net_feed_poll(&feed, 100);
        t1 = net_rdtscp();
        if (n < 0) {
            break;
        }
        if (n > 0 && n_cyc < count) {
            rx_cyc[n_cyc++] = t1 - t0;
        }
        while (got < count && net_race_acquire(&race, &pkt) == 0) {
            if (got == 0) {
                first_ns = pkt.rx_ns;
            }
            last_ns = pkt.rx_ns;
            (void)net_sink_consume(&sink, &pkt);
            net_race_release(&race);
            got++;
        }
        if (n == 0 && net_now_ns() - idle_ns > 2000000000ull && got > 0) {
            break;
        }
        if (n > 0) {
            idle_ns = net_now_ns();
        }
    }
    pthread_join(th, NULL);

    printf("RX BENCH  udp loopback\n");
    printf("count=%u  got=%u  slots=%u  size=%u  batch=%u  tsc_hz=%.6e\n",
           count, got, slots, size, batch, tsc.hz);
    printf("sink_ok=%" PRIu64 "  sink_bad=%" PRIu64 "  ring_drop=%" PRIu64 "\n",
           atomic_load_explicit(&sink.ok, memory_order_relaxed),
           atomic_load_explicit(&sink.bad, memory_order_relaxed),
           atomic_load_explicit(&stats.ring_drops, memory_order_relaxed));
    if (got > 1 && last_ns > first_ns) {
        double s = (double)(last_ns - first_ns) / 1e9;
        printf("window_s=%.6f  pps=%.1f\n", s, (double)got / s);
    }
    printf("\n");
    report("feed_poll bursts (rdtscp, includes recvmmsg + stamp + publish)",
           rx_cyc, n_cyc, &tsc);
    printf("\nloopback is not NIC latency.\n");

    free(rx_cyc);
    net_feed_close(&feed);
    net_ring_free(&ring);
    return 0;
}

#endif /* __linux__ */

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--mode ring|udp] [--count N] [--slots N] [--size N]\n"
            "          [--batch N] [--port N] [--cpu N]\n",
            prog);
}

int
main(int argc, char **argv)
{
    const char *mode = "ring";
    uint64_t count = DEFAULT_COUNT;
    uint64_t slots = DEFAULT_SLOTS;
    uint64_t size = DEFAULT_SIZE;
    uint64_t batch = DEFAULT_BATCH;
    uint64_t port = 39921;
    int cpu = -1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            mode = argv[++i];
        } else if (strcmp(argv[i], "--count") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &count) != 0 || count == 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--slots") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &slots) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--size") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &size) != 0 || size == 0 ||
                size > NET_PKT_MAX) {
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
        } else if (strcmp(argv[i], "--port") == 0) {
            if (net_require_arg(i, argc, argv[i], argv[0]) != 0) {
                return 1;
            }
            if (net_parse_u64(argv[++i], &port) != 0 || port == 0 ||
                port > 65535) {
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
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (net_pin_cpu(cpu) != 0) {
        return 1;
    }
    if (strcmp(mode, "ring") == 0) {
        return bench_ring((uint32_t)count, (uint32_t)slots, (uint16_t)size,
                          1000);
    }
#if defined(__linux__)
    if (strcmp(mode, "udp") == 0) {
        return bench_udp((uint32_t)count, (uint32_t)slots, (uint16_t)size,
                         (uint32_t)batch, (uint16_t)port);
    }
#endif
    fprintf(stderr, "unknown or unsupported mode: %s\n", mode);
    return 1;
}
