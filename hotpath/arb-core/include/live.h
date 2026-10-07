#ifndef ARB_CORE_LIVE_H
#define ARB_CORE_LIVE_H

#include "cycle.h"
#include "dlmm_cache.h"
#include "pool.h"
#include "pump.h"
#include "universe.h"

#include <stdint.h>

/*
 * LIVE — real-universe snapshot. Control plane writes the file.
 * The live binary only loads it. No universe_seed. No synthetic S.
 *
 * Per-pool arrays are heap-allocated for the generation size.
 * UNIV_POOL_HARD_MAX is a refuse bound (uint16 live.bin n), not a
 * favourite-pool selection cap.
 */

#define LIVE_MAGIC          0x4530314cU /* "L10E" */
#define LIVE_VER            1u
#define LIVE_VER2           2u
#define UNIV_POOL_HARD_MAX  65535u
#define UNIV_ROUTE_HARD_MAX 262144u
#define LIVE_POOL_MAX       UNIV_POOL_HARD_MAX
#define LIVE_BIN_MAX        DLMM_CACHE_WINDOW
#define LIVE_ROUTE_CAP      UNIV_ROUTE_HARD_MAX

typedef struct {
    uint8_t  protocol;
    uint8_t  pubkey[32];
    uint8_t  mint_x[32];
    uint8_t  mint_y[32];
    uint8_t  vault_x[32];
    uint8_t  vault_y[32];
} live_meta_t;

typedef struct {
    live_meta_t   *meta;
    dlmm_cache_t   dlmm;
    pump_state_t  *pump;
    uint8_t       *pump_live;
    amm2_state_t  *amm2;
    clmm_state_t  *clmm;
    uint8_t       *extra_live;
    pool_table_t   keys;
    universe_t     routes;
    uint32_t      n;
    uint32_t      cap;
    uint32_t      n_dlmm;
    uint32_t      n_pump;
    uint32_t      n_clmm;
    uint32_t      n_cpmm;
    uint32_t      n_damm;
    uint32_t      n_orca;
    uint16_t      file_ver;
    uint64_t      slot;
    uint64_t      state_version;
} live_univ_t;

int live_univ_init(live_univ_t *u);
int live_univ_reserve(live_univ_t *u, uint32_t cap);
void live_univ_free(live_univ_t *u);
int live_univ_load(live_univ_t *u, const char *path);

/* Refuse to start if any compiled route0 violates the mint contract. */
int live_univ_assert_route0(const live_univ_t *u);

int live_dlmm_quote(const live_univ_t *u, uint32_t pool_idx,
                    uint64_t amount_in, uint8_t swap_for_y,
                    dlmm_quote_t *out);
int live_pump_quote(const live_univ_t *u, uint32_t pool_idx,
                    uint64_t amount_in, uint8_t direction,
                    pump_quote_t *out);

int live_fill_univ_state(const live_univ_t *u, univ_state_t *st);

#endif /* ARB_CORE_LIVE_H */
