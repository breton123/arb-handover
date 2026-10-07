#ifndef FIRM_INGRESS_RESOLVE_H
#define FIRM_INGRESS_RESOLVE_H

#include "ingress/tx.h"
#include "state/compact.h"

#include <stdint.h>

/*
 * Map instruction pubkeys onto CompactState integer IDs.
 * Missing watched rows stay STATE_ACCT_NONE (deps_lookup fail-closes).
 */

#define RESOLVE_OK        0
#define RESOLVE_NO_POOL   1

int ingress_resolve(compact_state_t *st, ordered_tx_t *tx);

#endif /* FIRM_INGRESS_RESOLVE_H */
