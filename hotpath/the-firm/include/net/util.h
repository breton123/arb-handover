#ifndef FIRM_NET_UTIL_H
#define FIRM_NET_UTIL_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sched.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#endif

static inline int
net_parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || s[0] == '\0' || out == NULL) {
        return -1;
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static inline int
net_require_arg(int i, int argc, const char *flag, const char *prog)
{
    if (i + 1 >= argc) {
        fprintf(stderr, "%s: %s requires a value\n", prog, flag);
        return -1;
    }
    return 0;
}

/* cpu == -1 means do not pin. */
static inline int
net_pin_cpu(int cpu)
{
#if defined(__linux__)
    cpu_set_t set;

    if (cpu < 0) {
        return 0;
    }
    if (cpu >= CPU_SETSIZE) {
        fprintf(stderr, "cpu %d is out of range (CPU_SETSIZE=%d)\n",
                cpu, CPU_SETSIZE);
        return -1;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        return -1;
    }
    return 0;
#else
    (void)cpu;
    return 0;
#endif
}

static inline int
net_lock_memory(void)
{
#if defined(__linux__)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        return -1;
    }
    return 0;
#else
    return -1;
#endif
}

static inline int
net_disk_free_bytes(const char *path, uint64_t *out)
{
#if defined(__linux__)
    struct statvfs st;

    if (path == NULL || out == NULL) {
        return -1;
    }
    if (statvfs(path, &st) != 0) {
        perror("statvfs");
        return -1;
    }
    *out = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    return 0;
#else
    (void)path;
    (void)out;
    return -1;
#endif
}

#endif /* FIRM_NET_UTIL_H */
