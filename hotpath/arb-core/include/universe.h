#ifndef ARB_CORE_UNIVERSE_H
#define ARB_CORE_UNIVERSE_H

#include <stddef.h>
#include <stdint.h>

#include "adapter.h"
#include "proto.h"

#define UNIV_HOP_MAX  3u

#define ROUTE_FAM_0_DLMM_PUMP  0u
#define ROUTE_FAM_1_CLMM_DLMM  1u
#define ROUTE_FAM_2_CPMM_DLMM  2u
#define ROUTE_FAM_3_DLMM_DAMM  3u
#define ROUTE_FAM_4_ORCA_DLMM  4u
#define ROUTE_FAM_5_DLMM_PUMP_TYPED  5u
#define ROUTE_FAM_6_SUPPORTED  6u
#define ROUTE_FAM_6_DLMM_DLMM  ROUTE_FAM_6_SUPPORTED
#define ROUTE_FAM_OTHER        255u

/* Supported execution venues. A hop outside this set is not executable. */
#define ROUTE_VENUE_SUPPORTED  (REL_N_DLMM | REL_N_PUMP)

#define UNIV_MASK_2  (REL_N_DLMM | REL_N_PUMP)
#define UNIV_MASK_3  (UNIV_MASK_2 | REL_N_CLMM)
#define UNIV_MASK_4  (UNIV_MASK_3 | REL_N_CPMM)
#define UNIV_MASK_5  (UNIV_MASK_4 | REL_N_DAMM)
#define UNIV_MASK_6  (UNIV_MASK_5 | REL_N_ORCA)

typedef struct {
    uint8_t  protocol;
    uint8_t  mint_x[32];
    uint8_t  mint_y[32];
    uint32_t state_idx;
    uint32_t route_off;
    uint32_t route_n;
} pool_meta_t;

typedef struct {
    uint8_t  n_hop;
    uint8_t  family;
    uint8_t  proto[UNIV_HOP_MAX];
    uint32_t pool[UNIV_HOP_MAX];
    uint8_t  dir[UNIV_HOP_MAX];
} compiled_route_t;

typedef struct {
    pool_meta_t      *pool;
    compiled_route_t *route;
    uint32_t         *rindex;
    uint32_t          n_pool;
    uint32_t          n_route;
    uint32_t          proto_mask;
    uint32_t          pool_cap;
    uint32_t          route_cap;
    uint32_t          rindex_cap;
} universe_t;

typedef struct {
    amm2_state_t *amm2;
    clmm_state_t *clmm;
    uint32_t      n;
} univ_state_t;

int universe_init(universe_t *u, uint32_t pool_cap, uint32_t route_cap);
void universe_free(universe_t *u);
int universe_add_pool(universe_t *u, uint8_t proto,
                      const uint8_t mint_x[32], const uint8_t mint_y[32]);
int universe_compile(universe_t *u, uint32_t proto_mask);

/* Route0 denomination contract. Reverse orientation is not supported.
 *   DLMM.X == Pump.base == TOKEN != WSOL
 *   DLMM.Y == Pump.quote == WSOL
 * 0 = ROUTE0_INCOMPATIBLE. */
int route0_pair_compatible(const uint8_t dlmm_x[32], const uint8_t dlmm_y[32],
                           const uint8_t pump_base[32],
                           const uint8_t pump_quote[32]);
int univ_state_init(univ_state_t *st, uint32_t n);
void univ_state_free(univ_state_t *st);
void universe_fill_synthetic(const universe_t *u, univ_state_t *st);
uint8_t route_family_of(const compiled_route_t *r);
/* 1 iff every hop is DLMM or Pump and n_hop is 2 or 3. */
int route_hops_supported(const compiled_route_t *r);
/* 1 iff the generic hop dispatcher may execute this compiled route. */
int route_executable(const compiled_route_t *r);
int universe_scale_dummy(universe_t *u, uint32_t n_dummy);
int universe_seed(universe_t *u, uint32_t n_ids);

#endif /* ARB_CORE_UNIVERSE_H */
