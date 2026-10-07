#include "ingress/tx.h"

#include <string.h>

void
ordered_tx_clear(ordered_tx_t *tx)
{
    uint16_t i;

    if (tx == NULL) {
        return;
    }
    memset(tx, 0, sizeof(*tx));
    for (i = 0; i < STATE_IX_MAX; i++) {
        tx->ix[i].pool_id = STATE_ACCT_NONE;
        tx->ix[i].src_token = STATE_ACCT_NONE;
        tx->ix[i].dst_token = STATE_ACCT_NONE;
        tx->ix[i].vault_base = STATE_ACCT_NONE;
        tx->ix[i].vault_quote = STATE_ACCT_NONE;
        tx->ix[i].fee_proto = STATE_ACCT_NONE;
        tx->ix[i].fee_creator = STATE_ACCT_NONE;
        tx->ix[i].src_sys = STATE_ACCT_NONE;
        tx->ix[i].dst_sys = STATE_ACCT_NONE;
    }
}
