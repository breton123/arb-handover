#ifndef ARB_EXEC_TX_TEMPLATE_H
#define ARB_EXEC_TX_TEMPLATE_H

#include "opportunity.h"
#include "route0.h"

#include <string.h>

/*
 * EXEC-002 — route-0 unsigned tx template.
 *
 * Offline compile bakes account keys, header, CU-limit ix, and the
 * route0 account-index vector. Hot path is memcpy + five stores.
 *
 * Legacy message. No ALT: 33 unique route keys + OUR_EXEC +
 * ComputeBudget = 35. Pump base/quote mints alias DLMM X/Y.
 *
 *   [1][sig 64 zero][header 3][nkeys][keys][blockhash][3 ixs]
 *
 * Patch points are compile-time constants. Do not rebuild metas.
 */

#define ROUTE0_TX_NKEYS          35u
#define ROUTE0_TX_N_RO_UNSIGNED  20u
#define ROUTE0_TX_LEN            1305u

#define ROUTE0_OFF_SIGNATURE     1u
#define ROUTE0_OFF_MESSAGE       65u
#define ROUTE0_MSG_LEN           1240u

#define ROUTE0_OFF_BLOCKHASH     1189u
#define ROUTE0_OFF_CU_PRICE      1234u
#define ROUTE0_OFF_DIRECTION     1288u
#define ROUTE0_OFF_AMOUNT_IN     1289u
#define ROUTE0_OFF_MIN_PROFIT    1297u

#define ROUTE0_OFF_NKEYS         68u
#define ROUTE0_OFF_KEYS          69u
#define ROUTE0_OFF_NIX           1221u
#define ROUTE0_OFF_ROUTE0_NACC   1243u
#define ROUTE0_OFF_ROUTE0_ACCS   1244u
#define ROUTE0_OFF_ROUTE0_DATA   1280u

#define ROUTE0_CU_LIMIT          400000u

/* ComputeBudget111111111111111111111111111111 */
#define ROUTE0_COMPUTE_BUDGET_ID { \
    0x03, 0x06, 0x46, 0x6f, 0xe5, 0x21, 0x17, 0x32, \
    0xff, 0xec, 0xad, 0xba, 0x72, 0xc3, 0x9b, 0xe7, \
    0xbc, 0x8c, 0xe5, 0xbb, 0xc5, 0xf7, 0x12, 0x6b, \
    0x2c, 0x43, 0x9b, 0x3a, 0x40, 0x00, 0x00, 0x00  \
}

typedef struct {
    uint8_t bytes[ROUTE0_TX_LEN];
} route0_tx_template_t;

int route0_tx_compile(const uint8_t *keys,
                      const uint8_t *our_exec,
                      uint32_t cu_limit,
                      route0_tx_template_t *out);

/*
 * memcpy template, then patch the five known offsets.
 * Caller owns out[ROUTE0_TX_LEN]. No heap.
 */
static inline int
route0_tx_patch(const route0_tx_template_t *t,
                const opportunity_t *opp,
                uint64_t min_profit,
                const uint8_t blockhash[32],
                uint64_t cu_price,
                uint8_t *out)
{
    if (t == NULL || opp == NULL || blockhash == NULL || out == NULL) {
        return -1;
    }
    if (!opp->valid || opp->route_id != ROUTE_DLMM_PUMP || opp->direction > 1u) {
        return -1;
    }
    memcpy(out, t->bytes, ROUTE0_TX_LEN);
    memcpy(out + ROUTE0_OFF_BLOCKHASH, blockhash, 32);
    memcpy(out + ROUTE0_OFF_CU_PRICE, &cu_price, 8);
    out[ROUTE0_OFF_DIRECTION] = opp->direction;
    memcpy(out + ROUTE0_OFF_AMOUNT_IN, &opp->amount_in, 8);
    memcpy(out + ROUTE0_OFF_MIN_PROFIT, &min_profit, 8);
    return 0;
}

#endif /* ARB_EXEC_TX_TEMPLATE_H */
