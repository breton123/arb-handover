#ifndef FIRM_NET_TIME_H
#define FIRM_NET_TIME_H

#include <stdint.h>
#include <time.h>

/*
 * Monotonic high-resolution clocks for ingest.
 *
 * now_ns():  CLOCK_MONOTONIC_RAW when available. First-arrival stamp.
 * rdtscp():  serialized TSC for intra-process stage timing. Calibrate
 *            at startup; do not treat it as a wall clock across machines.
 */

#if defined(CLOCK_MONOTONIC_RAW)
#define NET_CLOCK_ID CLOCK_MONOTONIC_RAW
#elif defined(CLOCK_MONOTONIC)
#define NET_CLOCK_ID CLOCK_MONOTONIC
#else
#define NET_CLOCK_ID CLOCK_REALTIME
#endif

static inline uint64_t
net_now_ns(void)
{
    struct timespec ts;

    clock_gettime(NET_CLOCK_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t
net_realtime_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

static inline uint64_t
net_rdtscp(void)
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned int aux;
    return (uint64_t)__rdtscp(&aux);
#else
    return net_now_ns();
#endif
}

typedef struct {
    double hz;
} net_tsc_clock_t;

static inline int
net_tsc_calibrate(net_tsc_clock_t *c)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };
    uint64_t t0, t1, ns0, ns1;

    if (c == NULL) {
        return -1;
    }
    t0 = net_rdtscp();
    ns0 = net_now_ns();
#if defined(__linux__)
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
#else
    nanosleep(&ts, NULL);
#endif
    t1 = net_rdtscp();
    ns1 = net_now_ns();
    if (t1 <= t0 || ns1 <= ns0) {
        return -1;
    }
    c->hz = (double)(t1 - t0) * 1e9 / (double)(ns1 - ns0);
    return 0;
}

static inline uint64_t
net_tsc_to_ns(const net_tsc_clock_t *c, uint64_t cyc)
{
    if (c == NULL || c->hz <= 0.0) {
        return 0;
    }
    return (uint64_t)((double)cyc * 1e9 / c->hz);
}

#endif /* FIRM_NET_TIME_H */
