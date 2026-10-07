#ifndef ARB_EXEC_UTIL_H
#define ARB_EXEC_UTIL_H

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static inline int
parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || s[0] == '\0') {
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
require_arg(int i, int argc, const char *flag, const char *prog)
{
    if (i + 1 >= argc) {
        fprintf(stderr, "%s: %s requires a value\n", prog, flag);
        return -1;
    }
    return 0;
}

static inline int
pin_cpu(int cpu)
{
    cpu_set_t set;

    if (cpu < 0) {
        return 0;
    }
    if (cpu >= CPU_SETSIZE) {
        fprintf(stderr, "cpu %d is out of range (CPU_SETSIZE=%d)\n", cpu, CPU_SETSIZE);
        return -1;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        return -1;
    }
    return 0;
}

static inline int
lock_memory(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        return -1;
    }
    return 0;
}

#endif /* ARB_EXEC_UTIL_H */
