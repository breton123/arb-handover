#ifndef ARB_CORE_OPPORTUNITY_H
#define ARB_CORE_OPPORTUNITY_H

#include <stdint.h>

/*
 * What leaves arb-core. Gross protocol profit only.
 * arb-exec owns tips, priority fees, and whether to fire.
 */

#define ROUTE_DLMM_PUMP  0u

typedef struct {
    uint32_t route_id;
    uint64_t amount_in;
    uint64_t amount_out;
    uint64_t gross_profit;
    uint8_t  direction;
    uint8_t  valid;
} opportunity_t;

#endif /* ARB_CORE_OPPORTUNITY_H */
