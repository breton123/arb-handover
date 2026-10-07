#ifndef ARB_CORE_WATCH_H
#define ARB_CORE_WATCH_H

#include "live.h"

#include <stdint.h>

#define WATCH_CAP          4096u
#define WATCH_ROLE_POOL    1u
#define WATCH_ROLE_VAULT   2u
#define WATCH_ROLE_BIN     3u
#define WATCH_ROLE_ORACLE  4u
#define WATCH_ROLE_BITMAP  5u

typedef struct {
    uint8_t  used;
    uint8_t  pk[32];
    uint8_t  protocol;
    uint8_t  role;
    uint32_t pool_idx;
} watch_ent_t;

typedef struct {
    watch_ent_t *ent;
    uint32_t     cap;
    uint32_t     n;
} watch_idx_t;

int watch_init(watch_idx_t *w);
int watch_init_cap(watch_idx_t *w, uint32_t cap);
void watch_free(watch_idx_t *w);
int watch_put(watch_idx_t *w, const uint8_t pk[32], uint32_t pool_idx,
              uint8_t protocol, uint8_t role);

/* 0 on hit. */
int watch_get(const watch_idx_t *w, const uint8_t pk[32], watch_ent_t *out);

/* Index every pricing-critical account in the loaded universe. */
int watch_from_univ(watch_idx_t *w, const live_univ_t *u);

#endif
