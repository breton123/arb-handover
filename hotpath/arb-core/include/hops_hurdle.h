#ifndef ARB_CORE_HOPS_HURDLE_H
#define ARB_CORE_HOPS_HURDLE_H

#include <stdint.h>
#include <string.h>

#define HOPS_SWQOS_FEE   150000ull
#define HOPS_SAFETY_FEE   50000ull
#define HOPS_SIG_FEE       5000ull
#define HOPS_CU_PRICE   1000000ull

static inline uint64_t hops_min_gross(uint64_t cu_limit)
{
    return HOPS_SIG_FEE + (cu_limit * HOPS_CU_PRICE) / 1000000ull
        + HOPS_SWQOS_FEE + HOPS_SAFETY_FEE;
}

static inline uint64_t hops_hurdle_for_seq(const char *seq)
{
    if (seq == NULL) {
        return hops_min_gross(200000ull);
    }
    if (strcmp(seq, "dlmm-dlmm") == 0) {
        return hops_min_gross(98132ull);
    }
    if (strcmp(seq, "dlmm-pump") == 0) {
        return hops_min_gross(157860ull);
    }
    if (strcmp(seq, "pump-dlmm") == 0) {
        return hops_min_gross(174496ull);
    }
    if (strcmp(seq, "pump-pump") == 0) {
        return hops_min_gross(200000ull);
    }
    if (strcmp(seq, "dlmm-dlmm-dlmm") == 0) {
        return hops_min_gross(200000ull);
    }
    if (strcmp(seq, "dlmm-dlmm-pump") == 0) {
        return hops_min_gross(200000ull);
    }
    if (strcmp(seq, "pump-dlmm-dlmm") == 0) {
        return hops_min_gross(200000ull);
    }
    return hops_min_gross(200000ull);
}

#endif
