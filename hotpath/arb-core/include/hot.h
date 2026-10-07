#ifndef ARB_CORE_HOT_H
#define ARB_CORE_HOT_H

#include "dlmm.h"
#include "live.h"
#include "opportunity.h"
#include "pump.h"
#include "universe.h"

/*
 * Hot path: pool_idx changed → routes_by_pool[pool_idx] → quote → size.
 * Latency must not scale with global route count.
 *
 * Executable = route_executable() ({DLMM,Pump} 2/3-hop).
 * Families 0/5/6 are template metadata, not a search permit.
 * route0 still uses cycle_size() as one candidate. amm2 never quotes it.
 */

typedef struct {
    uint8_t        protocol;
    dlmm_swap_ix_t dlmm;
    pump_swap_ix_t pump;
} hot_trigger_t;

/* opportunity_t stays frozen. Version rides beside it for the journal. */
typedef struct {
    opportunity_t opp;
    uint64_t      state_version;
} hot_decision_t;

int hot_quote_route(const universe_t *u, const univ_state_t *st,
                    uint32_t route_idx, uint64_t amount_in,
                    uint64_t *amount_out);

int hot_eval_pool(const universe_t *u, const univ_state_t *st,
                  uint32_t pool_idx, opportunity_t *best);

int hot_apply_pool(const universe_t *u, univ_state_t *st,
                   uint32_t pool_idx, uint64_t amount_in, int dir);

int hot_decode_trigger(uint8_t proto, const uint8_t *data, uint16_t len,
                       uint8_t swap_for_y, hot_trigger_t *out);

int hot_eval_live(const live_univ_t *u, uint32_t pool_idx,
                  const hot_trigger_t *n, opportunity_t *out);

int hot_decide(const live_univ_t *u, uint32_t pool_idx,
               const hot_trigger_t *n, hot_decision_t *out);

/* Confirmed stream only. Applies N to canonical S and bumps state_version. */
int hot_commit(live_univ_t *u, uint32_t pool_idx, const hot_trigger_t *n);

static inline int
hot_stale(const live_univ_t *u, uint64_t used_version)
{
    return u == NULL || used_version != u->state_version;
}

#endif /* ARB_CORE_HOT_H */
