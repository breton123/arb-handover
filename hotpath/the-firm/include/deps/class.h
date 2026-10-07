#ifndef FIRM_DEPS_CLASS_H
#define FIRM_DEPS_CLASS_H

#include "ingress/tx.h"
#include "state/compact.h"

/*
 * Possible-write classification, then residency lookup.
 * DEP_UNKNOWN is not a final apply reason; split at the raise site.
 */

#define DEP_IRRELEVANT                 0u
#define DEP_FAST_CUSTOM                1u
#define DEP_FALLBACK                   2u
#define DEP_UNKNOWN                    3u /* leftover only; lookup must replace */
#define DEP_NO_QUOTE_EFFECT            4u
#define DEP_MISSING_POOL_ROW           5u
#define DEP_MISSING_BASE_VAULT_ROW     6u
#define DEP_MISSING_QUOTE_VAULT_ROW    7u
#define DEP_MISSING_CONFIG             8u
#define DEP_MISSING_OTHER_ROW          9u
#define DEP_EXTRA_IX_TOUCHES_STATE     10u
#define DEP_EXTRA_IX_NO_STATE_OVERLAP  11u
#define DEP_UNKNOWN_CPI_EFFECT         12u
#define DEP_UNKNOWN_PUMP_KIND          13u

typedef struct {
    uint8_t  reason;
    uint16_t ix_index;
    uint8_t  account_index;
    uint8_t  writable;
    uint8_t  compact_row_present;
    uint8_t  compact_row_exact;
    uint8_t  account[32];
    uint8_t  program[32];
    uint8_t  required_by; /* DEP_NEED_* */
} dep_offend_t;

#define DEP_NEED_NONE         0u
#define DEP_NEED_POOL         1u
#define DEP_NEED_VAULT_BASE   2u
#define DEP_NEED_VAULT_QUOTE  3u
#define DEP_NEED_SRC_TOKEN    4u
#define DEP_NEED_DST_TOKEN    5u
#define DEP_NEED_FEE_PROTO    6u
#define DEP_NEED_FEE_CREATOR  7u
#define DEP_NEED_SRC_SYS      8u
#define DEP_NEED_DST_SYS      9u

typedef struct {
    uint8_t      tx_class;
    uint8_t      n_relevant;
    uint8_t      n_fast;
    uint8_t      n_fallback;
    uint8_t      n_unknown;
    uint8_t      have_off;
    uint8_t      ix_class[STATE_IX_MAX];
    dep_offend_t off;
} dep_report_t;

void deps_classify(const ordered_tx_t *tx, dep_report_t *out);

/*
 * Resolve write-set rows in CompactState. Extra outer ixs whose
 * writable accounts miss this pool's quote/state set become
 * DEP_NO_QUOTE_EFFECT. Missing rows and overlapping extra ixs
 * become a specific fail class — never DEP_UNKNOWN.
 */
int deps_lookup(const compact_state_t *st, const ordered_tx_t *tx,
                dep_report_t *inout);

const char *deps_class_name(uint8_t c);
const char *deps_need_name(uint8_t need);
int deps_blocks_fast(uint8_t c);

#endif /* FIRM_DEPS_CLASS_H */
