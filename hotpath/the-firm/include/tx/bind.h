#ifndef FIRM_TX_BIND_H
#define FIRM_TX_BIND_H

#include "ingress/tx.h"
#include "tx/view.h"

/*
 * Fill OrderedTx from a resolved view. Pump AMM discs only; bonding
 * is not FAST_CUSTOM. No apply.
 */

int txview_bind(const tx_view_t *v, uint64_t slot, ordered_tx_t *out);

#endif
