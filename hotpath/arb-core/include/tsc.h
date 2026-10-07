#ifndef ARB_CORE_TSC_H
#define ARB_CORE_TSC_H

#include <stdint.h>
#include <time.h>

static inline uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

static inline uint64_t rdtscp(void)
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned int aux;
    return (uint64_t)__rdtscp(&aux);
#else
    return now_ns();
#endif
}

struct tsc_clock {
    double hz;
};

static inline int tsc_calibrate(struct tsc_clock *c)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };
    uint64_t t0, t1, ns0, ns1;

    t0 = rdtscp();
    ns0 = now_ns();
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    t1 = rdtscp();
    ns1 = now_ns();
    if (t1 <= t0 || ns1 <= ns0) {
        return -1;
    }
    c->hz = (double)(t1 - t0) * 1e9 / (double)(ns1 - ns0);
    return 0;
}

static inline uint64_t tsc_to_ns(const struct tsc_clock *c, uint64_t cyc)
{
    if (c->hz <= 0.0) {
        return 0;
    }
    return (uint64_t)((double)cyc * 1e9 / c->hz);
}

#endif /* ARB_CORE_TSC_H */
