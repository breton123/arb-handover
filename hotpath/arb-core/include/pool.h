#ifndef ARB_CORE_POOL_H
#define ARB_CORE_POOL_H

#include "tx.h"

#include <stdint.h>

typedef struct {
    uint8_t  protocol;
    uint32_t pool_idx;
} affected_pool_t;

typedef struct {
    uint8_t  key[32];
    uint8_t  protocol;
    uint32_t idx;
} pool_slot_t;

typedef struct {
    pool_slot_t *slot;
    uint32_t cap;
    uint32_t n;
} pool_table_t;

int pool_table_init(pool_table_t *t, uint32_t min_cap);
void pool_table_free(pool_table_t *t);

/* Insert unique key. idx is assigned in insert order. Returns 0 if stored. */
int pool_table_put(pool_table_t *t, const uint8_t *key, uint8_t protocol);

/* 0 on hit. */
int pool_table_get(const pool_table_t *t, const uint8_t *key,
                   affected_pool_t *out);

int pool_table_load(pool_table_t *t, const char *path);
int pool_table_save(const pool_table_t *t, const char *path);

/*
 * CORE-001 fast path. Returns 0 hit, 1 framed dex ix but unknown pool,
 * 2 no message/keys.
 */
int affected_from_payload(const uint8_t *payload, uint16_t len,
                          const pool_table_t *t, affected_pool_t *out);

/*
 * CORE-002 miss path only: extra program alignments, then byte-offset
 * txn scan. Call after affected_from_payload returns non-zero.
 */
int affected_offset_scan(const uint8_t *payload, uint16_t len,
                         const pool_table_t *t, affected_pool_t *out);

#endif /* ARB_CORE_POOL_H */
