#ifndef ARB_CORE_UNIVERSE_GEN_H
#define ARB_CORE_UNIVERSE_GEN_H

#include "live.h"
#include "watch.h"

#include <stdint.h>

/*
 * Immutable heap generation. Control plane builds one, then publishes
 * a pointer. The hot path only reads. Designed for tens of thousands
 * of pools — not a bumped 256/1024 favourite-pool cap.
 */

typedef struct universe_gen {
    uint32_t     gen_id;
    uint32_t     n_pools;
    uint8_t      immutable;
    live_univ_t  univ;
    watch_idx_t  watch;
} universe_gen_t;

int universe_gen_build(universe_gen_t *g, const char *live_path, uint32_t gen_id);
void universe_gen_free(universe_gen_t *g);

/* Atomically publish g. Returns the previous generation for deferred free. */
universe_gen_t *universe_gen_publish(universe_gen_t *g);

/* Hot path: acquire current generation. Never writes. */
const universe_gen_t *universe_gen_current(void);

#endif
