#ifndef ARB_EXEC_CTRL_H
#define ARB_EXEC_CTRL_H

#include "nonce.h"
#include "tx_template.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Control plane. RPC lives here, or in the script that writes the bin.
 * The opportunity path only claims a nonce and reads these numbers.
 *
 *   nonce_load × 64 → READY
 *   nonce_claim     → IN_FLIGHT
 *   confirm / fail  → nonce_reload → READY
 *
 * Fees: startup / this plane writes cu_price and min_profit.
 * Hot path patches the frozen offsets. No RPC on that path.
 */

#define CTRL_BIN_MAGIC  0x314C5443u /* CTL1 */
#define CTRL_BIN_VER    1u
#define CTRL_BIN_HDR    24u
#define CTRL_BIN_REC    64u
#define CTRL_BIN_LEN    (CTRL_BIN_HDR + (uint32_t)NONCE_POOL_N * CTRL_BIN_REC)

typedef struct {
    uint8_t pubkey[32];
    uint8_t hash[32];
} ctrl_nonce_rec_t;

typedef struct {
    uint64_t cu_price;
    uint64_t min_profit;
} ctrl_fees_t;

void ctrl_fees_clear(ctrl_fees_t *f);

int ctrl_fees_set(ctrl_fees_t *f, uint64_t cu_price, uint64_t min_profit);

int ctrl_nonce_load_all(nonce_pool_t *p, const ctrl_nonce_rec_t *rec, uint32_t n);

int ctrl_nonce_finish(nonce_pool_t *p, uint32_t idx, const uint8_t hash[32]);

int ctrl_bin_read(const uint8_t *buf, size_t len,
                  ctrl_nonce_rec_t rec[NONCE_POOL_N], ctrl_fees_t *fees);

int ctrl_bin_write(uint8_t *buf, size_t cap,
                   const ctrl_nonce_rec_t rec[NONCE_POOL_N],
                   const ctrl_fees_t *fees, size_t *out_len);

/*
 * Opportunity path. Claim already happened. No RPC, no fee fetch.
 */
static inline int
ctrl_patch(const route0_tx_template_t *t,
           const opportunity_t *opp,
           const ctrl_fees_t *fees,
           const nonce_claim_t *claim,
           uint8_t *out)
{
    if (fees == NULL || claim == NULL) {
        return -1;
    }
    return route0_tx_patch(t, opp, fees->min_profit, claim->hash,
                           fees->cu_price, out);
}

#endif /* ARB_EXEC_CTRL_H */
