#ifndef ARB_CORE_CLASSIFY_N_H
#define ARB_CORE_CLASSIFY_N_H

#include <stddef.h>
#include <stdint.h>

/*
 * CORE-009 multi-program classifier. Locked classify.c stays 2-ID.
 * n_ids = 2..6 in expansion order: DLMM, Pump, CLMM, CPMM, DAMM, Orca.
 */

uint32_t classify_n(const uint8_t *payload, size_t len, uint32_t n_ids);

#endif /* ARB_CORE_CLASSIFY_N_H */
