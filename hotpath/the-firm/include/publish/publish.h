#ifndef FIRM_PUBLISH_PUBLISH_H
#define FIRM_PUBLISH_PUBLISH_H

#include "deps/class.h"
#include "ingress/tx.h"
#include "state/compact.h"
#include "transitions/overlay.h"

/*
 * Atomic per-transaction publication. No partial-leg publish.
 * Writes state_cert_t on success.
 */

int state_publish(compact_state_t *st, const tx_overlay_t *ov,
                  const ordered_tx_t *tx, const dep_report_t *dep);

int state_publish_irrelevant(compact_state_t *st, const ordered_tx_t *tx,
                             const dep_report_t *dep);

#endif /* FIRM_PUBLISH_PUBLISH_H */
