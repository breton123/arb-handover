#ifndef ARB_CORE_DLMM_CACHE_H
#define ARB_CORE_DLMM_CACHE_H

#include "dlmm.h"

#include <stdint.h>

/*
 * CORE-004 — dense DLMM hot cache.
 *
 * pools[pool_idx] is a direct index. No hash on the hot path.
 * Each pool keeps a fixed window [active_id - K, active_id + K] from the
 * last confirmed refresh. Missing bin on the walk → fail closed.
 *
 * Trigger N never writes canonical S. predict() copies, apply() on the
 * copy, caller decides commit vs discard.
 */

#define DLMM_CACHE_K           16
#define DLMM_CACHE_WINDOW      (2 * DLMM_CACHE_K + 1)
/* Hard refuse only. Cache length is heap-reserved per generation. */
#define DLMM_CACHE_MAX_POOLS   65535

#define DLMM_CAP_MAGIC         0x43303034u
#define DLMM_CAP_VER           1
#define DLMM_CAP_BINS          64

typedef struct {
    int32_t  id;
    uint64_t amount_x;
    uint64_t amount_y;
} dlmm_hot_bin_t;

typedef struct {
    uint8_t              occupied;
    uint8_t              status;
    uint16_t             bin_step;
    int32_t              active_id;
    int32_t              window_center;
    dlmm_static_params_t parameters;
    dlmm_vparams_t       v_parameters;
    uint64_t             reserve_x;
    uint64_t             reserve_y;
    int64_t              now_ts;
    dlmm_hot_bin_t       bin[DLMM_CACHE_WINDOW];
    uint8_t              live[DLMM_CACHE_WINDOW];
} dlmm_pool_hot_t;

typedef struct {
    dlmm_pool_hot_t *pool;
    uint32_t         n;
    uint32_t         cap;
} dlmm_cache_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t rec_bytes;
    uint32_t nrec;
    uint32_t reserved;
} dlmm_cap_hdr_t;

typedef struct {
    uint32_t             pool_idx;
    uint8_t              swap_for_y;
    uint8_t              pad[3];
    uint64_t             amount_in;
    uint64_t             min_out;
    int64_t              now_ts;
    int32_t              before_active;
    int32_t              after_active;
    uint16_t             bin_step;
    uint16_t             nbin_before;
    uint16_t             nbin_after;
    uint8_t              status;
    uint8_t              crossed;
    dlmm_static_params_t parameters;
    dlmm_vparams_t       v_before;
    dlmm_vparams_t       v_after;
    uint64_t             before_rx;
    uint64_t             before_ry;
    uint64_t             after_rx;
    uint64_t             after_ry;
    dlmm_hot_bin_t       bins_before[DLMM_CAP_BINS];
    dlmm_hot_bin_t       bins_after[DLMM_CAP_BINS];
} dlmm_cap_rec_t;

typedef struct {
    uint8_t  match;
    uint8_t  insufficient;
    uint16_t crossed;
    uint32_t hot_bytes;
} dlmm_cap_verdict_t;

void dlmm_cache_init(dlmm_cache_t *c);
int dlmm_cache_reserve(dlmm_cache_t *c, uint32_t cap);
void dlmm_cache_free(dlmm_cache_t *c);

/* Install confirmed state. Window recenters on s->active_id. Bins outside ±K drop. */
int dlmm_cache_put(dlmm_cache_t *c, uint32_t pool_idx, const dlmm_state_t *s);

const dlmm_pool_hot_t *dlmm_cache_get(const dlmm_cache_t *c, uint32_t pool_idx);

/* Speculative: copy hot → apply → spec. Canonical cache is not written. */
int dlmm_cache_predict(const dlmm_cache_t *c, uint32_t pool_idx,
                       const dlmm_swap_ix_t *ix,
                       dlmm_pool_hot_t *spec,
                       dlmm_apply_result_t *res);

/* Confirmed path only. */
int dlmm_cache_commit(dlmm_cache_t *c, uint32_t pool_idx,
                      const dlmm_pool_hot_t *spec);

int dlmm_hot_slot(const dlmm_pool_hot_t *h, int32_t id);

void dlmm_hot_to_state(const dlmm_pool_hot_t *h, dlmm_state_t *s);
int dlmm_state_into_hot(const dlmm_state_t *s, dlmm_pool_hot_t *h);

/* Economically relevant fields (active, reserves, vol acc, live bin amounts). */
int dlmm_hot_econ_eq(const dlmm_pool_hot_t *a, const dlmm_pool_hot_t *b);

uint32_t dlmm_hot_bytes(const dlmm_pool_hot_t *h);

int dlmm_cap_save(const char *path, const dlmm_cap_rec_t *recs, uint32_t n);
int dlmm_cap_load(const char *path, dlmm_cap_rec_t **recs, uint32_t *n);
void dlmm_cap_free(dlmm_cap_rec_t *recs);

int dlmm_cap_check(const dlmm_cap_rec_t *rec, dlmm_cap_verdict_t *v);

#endif /* ARB_CORE_DLMM_CACHE_H */
