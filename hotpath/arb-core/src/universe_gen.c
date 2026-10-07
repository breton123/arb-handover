#include "universe_gen.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic(universe_gen_t *) g_cur = NULL;

static uint32_t
watch_cap_for(uint32_t n_pools)
{
    uint64_t n = (uint64_t)n_pools * 8ull;
    uint32_t cap = 4096u;

    if (n > (uint64_t)UNIV_POOL_HARD_MAX * 8ull) {
        n = (uint64_t)UNIV_POOL_HARD_MAX * 8ull;
    }
    while ((uint64_t)cap < n) {
        if (cap > (UINT32_MAX / 2u)) {
            return UINT32_MAX;
        }
        cap *= 2u;
    }
    return cap;
}

int
universe_gen_build(universe_gen_t *g, const char *live_path, uint32_t gen_id)
{
    uint32_t wcap;

    if (g == NULL || live_path == NULL) {
        return -1;
    }
    memset(g, 0, sizeof(*g));
    g->gen_id = gen_id;
    if (live_univ_init(&g->univ) != 0 || live_univ_load(&g->univ, live_path) != 0) {
        universe_gen_free(g);
        return -1;
    }
    g->n_pools = g->univ.n;
    wcap = watch_cap_for(g->n_pools);
    if (watch_init_cap(&g->watch, wcap) != 0
        || watch_from_univ(&g->watch, &g->univ) != 0) {
        universe_gen_free(g);
        return -1;
    }
    g->immutable = 1;
    return 0;
}

void
universe_gen_free(universe_gen_t *g)
{
    if (g == NULL) {
        return;
    }
    watch_free(&g->watch);
    live_univ_free(&g->univ);
    memset(g, 0, sizeof(*g));
}

universe_gen_t *
universe_gen_publish(universe_gen_t *g)
{
    return atomic_exchange_explicit(&g_cur, g, memory_order_acq_rel);
}

const universe_gen_t *
universe_gen_current(void)
{
    return atomic_load_explicit(&g_cur, memory_order_acquire);
}
