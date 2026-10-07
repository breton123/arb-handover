#ifndef ARB_EXEC_ROUTE0_V0_H
#define ARB_EXEC_ROUTE0_V0_H

#include "opportunity.h"
#include "route0.h"
#include "sign.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * EXEC-LIVE-002B — v0 + one ALT. EXEC-002 legacy template stays frozen.
 *
 * OUR_EXEC still sees EXEC-001 account order 0..34.
 * Bitmap / host_fee optional-none = DLMM program id (no dummy PDAs).
 * Remaining 35 = Token-2022, 36..57 = live Pump IDL metas.
 *
 * Static message keys: payer + program ids.
 * Route / pool accounts live in one ALT.
 */

#define V0_N_STATIC    11u
#define V0_N_ALT_WR    14u
#define V0_N_ALT_RO     9u
#define V0_N_IXACC     58u
#define V0_TX_LEN     619u
#define V0_MSG_LEN    554u

#define V0_OFF_SIGNATURE     1u
#define V0_OFF_MESSAGE      65u
#define V0_OFF_BLOCKHASH   422u
#define V0_OFF_CU_PRICE    467u
#define V0_OFF_DIRECTION   544u
#define V0_OFF_AMOUNT_IN   545u
#define V0_OFF_MIN_PROFIT  553u

#define V0_STATIC_AUTHORITY   0u
#define V0_STATIC_OUR_EXEC    1u
#define V0_STATIC_CU          2u
#define V0_STATIC_TOKENKEG    3u
#define V0_STATIC_SYSTEM      4u
#define V0_STATIC_ATA         5u
#define V0_STATIC_MEMO        6u
#define V0_STATIC_DLMM        7u
#define V0_STATIC_PUMP        8u
#define V0_STATIC_TOKEN2022   9u
#define V0_STATIC_FEE_PROG   10u

typedef struct {
    uint8_t bytes[V0_TX_LEN];
} route0_v0_template_t;

int route0_v0_compile(const uint8_t *static_keys,
                      const uint8_t *alt,
                      const uint8_t *alt_wr,
                      const uint8_t *alt_ro,
                      uint32_t cu_limit,
                      route0_v0_template_t *out);

static inline int
route0_v0_patch(const route0_v0_template_t *t,
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
    memcpy(out, t->bytes, V0_TX_LEN);
    memcpy(out + V0_OFF_BLOCKHASH, blockhash, 32);
    memcpy(out + V0_OFF_CU_PRICE, &cu_price, 8);
    out[V0_OFF_DIRECTION] = opp->direction;
    memcpy(out + V0_OFF_AMOUNT_IN, &opp->amount_in, 8);
    memcpy(out + V0_OFF_MIN_PROFIT, &min_profit, 8);
    return 0;
}

static inline int
route0_v0_sign(const route0_signer_t *s, uint8_t *tx)
{
    if (s == NULL || tx == NULL) {
        return -1;
    }
    return exec_sign(s, tx + V0_OFF_SIGNATURE,
                     tx + V0_OFF_MESSAGE, V0_MSG_LEN);
}

#endif /* ARB_EXEC_ROUTE0_V0_H */
