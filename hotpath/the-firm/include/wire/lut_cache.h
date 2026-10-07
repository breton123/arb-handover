#ifndef FIRM_WIRE_LUT_CACHE_H
#define FIRM_WIRE_LUT_CACHE_H

#include "tx/view.h"
#include "wire/classify.h"
#include "wire/lut_parse.h"

#include <stdint.h>
#include <stdio.h>

/*
 * Warm full tables + sparse (table, index) → address + off-path miss list.
 * Hot path is lookup only. Fetch is cold. Generation bumps on install.
 *
 * Miss → fail closed. Do not apply. Do not treat LUT as process-wide
 * fatal. Pools named in static keys of an unresolved tx may go DIRTY.
 */

#define LUT_SPARSE_MAX  65536u
#define LUT_MISS_MAX    512u
#define LUT_QUAR_MAX    64u

typedef struct {
    uint8_t  table[32];
    uint8_t  addr[32];
    uint8_t  idx;
    uint8_t  live;
    uint8_t  quar;
    uint8_t  have_meta;
    uint8_t  start_index;
    uint64_t last_extended_slot;
    uint64_t deactivation_slot;
} lut_sparse_ent_t;

typedef struct {
    wire_lut_tab_t   owned;
    wire_lut_tab_t  *full;
    lut_account_t    meta[WIRE_LUT_MAX];
    lut_sparse_ent_t sp[LUT_SPARSE_MAX];
    uint8_t          quar[LUT_QUAR_MAX][32];
    uint8_t          miss[LUT_MISS_MAX][32];
    uint32_t         n_sp;
    uint32_t         n_quar;
    uint32_t         n_miss;
    uint32_t         n_miss_written;
    uint64_t         generation;
    uint64_t         n_lookup_ok;
    uint64_t         n_lookup_fail;
    uint64_t         n_learn;
    uint64_t         n_install;
    uint8_t          sealed; /* 1 = no learn, no miss-list, no jsonl poll */
} lut_cache_t;

void lut_cache_clear(lut_cache_t *c);
void lut_cache_use_full(lut_cache_t *c, wire_lut_tab_t *tab);

int lut_cache_lookup(const lut_cache_t *c, const uint8_t table[32],
                     uint8_t idx, uint64_t slot, uint8_t out[32]);

int lut_cache_learn_sparse(lut_cache_t *c, const uint8_t table[32],
                           uint8_t idx, const uint8_t addr[32],
                           const lut_account_t *meta);

int lut_cache_install_account(lut_cache_t *c, const uint8_t table[32],
                              const lut_account_t *acc);

void lut_cache_note_view(lut_cache_t *c, const tx_view_t *v, uint64_t slot);

int lut_cache_load_jsonl(lut_cache_t *c, const char *path);
int lut_cache_flush_miss(lut_cache_t *c, FILE *fp);
void lut_cache_prune_miss(lut_cache_t *c);

#endif
