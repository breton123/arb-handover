#ifndef ARB_CORE_PAPER_H
#define ARB_CORE_PAPER_H

#include "hot.h"
#include "live.h"

/*
 * PAPER — quote every compiled route on real state.
 * route0 uses frozen cycle_size. Other supported hops use live kernels.
 * Families 0/5/6 are metadata. Permission is route_executable().
 * EXEC_MISSING only if a hop uses an unsupported venue (family 255).
 */

#define PAPER_SIGNED                 0u
#define PAPER_EXEC_FAMILY_MISSING    1u
#define PAPER_STATE_INSUFFICIENT     2u

typedef struct {
    opportunity_t opp;
    uint32_t      pool_idx;
    uint32_t      route_id;
    uint8_t       family;
    uint8_t       n_hop;
    uint8_t       proto[UNIV_HOP_MAX];
    uint8_t       searchable;
    uint8_t       signed_ready;
    uint8_t       reason;
    uint8_t       state_ok;
} paper_opp_t;

int paper_quote_hop(const live_univ_t *u, const univ_state_t *st,
                    uint32_t pool_idx, int dir, uint64_t ain, uint64_t *aout);

int paper_quote_route(const live_univ_t *u, const univ_state_t *st,
                      uint32_t route_idx, uint64_t amount_in,
                      uint64_t *amount_out);

int paper_eval_pool(const live_univ_t *u, const univ_state_t *st,
                    uint32_t pool_idx, paper_opp_t *best);

int paper_eval_trigger(const live_univ_t *u, univ_state_t *st,
                       uint32_t pool_idx, const hot_trigger_t *n,
                       paper_opp_t *best);

#endif /* ARB_CORE_PAPER_H */
