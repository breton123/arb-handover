#ifndef ARB_EXEC_OPPORTUNITY_H
#define ARB_EXEC_OPPORTUNITY_H

#include <stdint.h>

/*
 * Lockstep with arb-core. Do not widen. Gross protocol profit only.
 * arb-exec owns min_profit, tips, and whether to fire.
 */

#ifndef ARB_CORE_OPPORTUNITY_H
#define ROUTE_DLMM_PUMP  0u

typedef struct {
    uint32_t route_id;
    uint64_t amount_in;
    uint64_t amount_out;
    uint64_t gross_profit;
    uint8_t  direction;
    uint8_t  valid;
} opportunity_t;
#endif

#endif /* ARB_EXEC_OPPORTUNITY_H */
