#ifndef ARB_EXEC_ROUTE_FAM_H
#define ARB_EXEC_ROUTE_FAM_H

#ifndef ARB_CORE_OPPORTUNITY_H
#include "opportunity.h"
#endif
#include "tx_template.h"

/*
 * route0 stays FROZEN. Families 1–4 are extra templates with the same
 * memcpy+patch offsets. Live CLMM/CPMM/DAMM/Orca account vectors later.
 */

#define ROUTE_FAM_N  5u

typedef struct {
    route0_tx_template_t tmpl[ROUTE_FAM_N];
    uint8_t              have[ROUTE_FAM_N];
} route_fam_t;

int route_fam_init(route_fam_t *f);

static inline int
route_fam_patch(const route_fam_t *f, uint8_t family,
                const opportunity_t *opp, uint64_t min_profit,
                const uint8_t *blockhash, uint64_t cu_price, uint8_t *out)
{
    const route0_tx_template_t *t;

    if (f == NULL || opp == NULL || blockhash == NULL || out == NULL
        || !opp->valid || opp->direction > 1u) {
        return -1;
    }
    if (family >= ROUTE_FAM_N || !f->have[family]) {
        family = 0;
    }
    t = &f->tmpl[family];
    memcpy(out, t->bytes, ROUTE0_TX_LEN);
    memcpy(out + ROUTE0_OFF_BLOCKHASH, blockhash, 32);
    memcpy(out + ROUTE0_OFF_CU_PRICE, &cu_price, 8);
    out[ROUTE0_OFF_DIRECTION] = opp->direction;
    memcpy(out + ROUTE0_OFF_AMOUNT_IN, &opp->amount_in, 8);
    memcpy(out + ROUTE0_OFF_MIN_PROFIT, &min_profit, 8);
    return 0;
}

#endif /* ARB_EXEC_ROUTE_FAM_H */
