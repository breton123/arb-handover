#include "ingress/resolve.h"

#include <string.h>

static int
map_tok(const compact_state_t *st, const uint8_t pk[32], uint32_t *id)
{
    if (compact_token_find(st, pk, id) != 0) {
        *id = STATE_ACCT_NONE;
        return -1;
    }
    return 0;
}

int
ingress_resolve(compact_state_t *st, ordered_tx_t *tx)
{
    uint16_t i;

    if (st == NULL || tx == NULL) {
        return RESOLVE_NO_POOL;
    }
    for (i = 0; i < tx->n_ix; i++) {
        ordered_ix_t *ix = &tx->ix[i];
        uint8_t n = tx->acc_n[i];
        const uint8_t *k0 = NULL;

        if (n > 0 && tx->acc_ix[i][0] < tx->n_keys) {
            k0 = tx->key[tx->acc_ix[i][0]];
        }
        if (ix->proto == PROTO_PUMP && k0 != NULL) {
            if (compact_pool_find(st, k0, &ix->pool_id) != 0) {
                ix->pool_id = STATE_ACCT_NONE;
            }
            if (n > 5) {
                (void)map_tok(st, tx->key[tx->acc_ix[i][5]], &ix->src_token);
            }
            if (n > 6) {
                (void)map_tok(st, tx->key[tx->acc_ix[i][6]], &ix->dst_token);
            }
            if (n > 7) {
                (void)map_tok(st, tx->key[tx->acc_ix[i][7]], &ix->vault_base);
            }
            if (n > 8) {
                (void)map_tok(st, tx->key[tx->acc_ix[i][8]], &ix->vault_quote);
            }
        } else if (ix->proto == PROTO_ATA && n > 1) {
            /* acc[0] is the fee payer (system account), not a token. */
            (void)map_tok(st, tx->key[tx->acc_ix[i][1]], &ix->dst_token);
        } else if (ix->proto == PROTO_TOKEN && n > 0) {
            if (n > 0) {
                (void)map_tok(st, tx->key[tx->acc_ix[i][0]], &ix->src_token);
            }
            if (n > 1) {
                (void)map_tok(st, tx->key[tx->acc_ix[i][1]], &ix->dst_token);
            }
        } else if (ix->proto == PROTO_SYSTEM && n > 0) {
            if (compact_sys_find(st, tx->key[tx->acc_ix[i][0]],
                                 &ix->src_sys) != 0) {
                ix->src_sys = STATE_ACCT_NONE;
            }
            if (n > 1 && compact_sys_find(st, tx->key[tx->acc_ix[i][1]],
                                          &ix->dst_sys) != 0) {
                ix->dst_sys = STATE_ACCT_NONE;
            }
        }
    }
    return RESOLVE_OK;
}
