#ifndef FIRM_TX_RESOLVE_H
#define FIRM_TX_RESOLVE_H

#include "tx/view.h"
#include "wire/classify.h"
#include "wire/lut_cache.h"

/*
 * Canonical v0 loaded-key order: static keys, then every table's
 * writable addresses in descriptor order, then every table's
 * readonly addresses. Missing table or OOB index → fail closed.
 */

#define TXV_RES_OK   0
#define TXV_RES_LUT  1
#define TXV_RES_BAD  2

int txview_resolve(tx_view_t *v, const wire_lut_tab_t *luts);

int txview_resolve_at(tx_view_t *v, lut_cache_t *c, uint64_t slot);

#endif
