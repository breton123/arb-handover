#include "live.h"
#include "paper.h"
#include "universe_gen.h"
#include "watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", m); return 1; } } while (0)

int
main(void)
{
    live_univ_t u;
    universe_gen_t *a;
    universe_gen_t *prev;

    CHECK(sizeof(live_univ_t) < 1024u, "live_univ_t is pointer-sized, not a 256-pool blob");
    CHECK(UNIV_POOL_HARD_MAX >= 528u, "hard max covers the proven market hour");
    CHECK(UNIV_POOL_HARD_MAX >= 10000u, "designed for tens of thousands");

    memset(&u, 0, sizeof(u));
    CHECK(live_univ_init(&u) == 0, "init");
    CHECK(live_univ_reserve(&u, 528) == 0, "reserve 528");
    CHECK(u.cap >= 528, "cap");
    CHECK(u.meta != NULL && u.pump != NULL, "heap arrays");
    CHECK(dlmm_cache_reserve(&u.dlmm, 528) == 0, "dlmm reserve");
    live_univ_free(&u);

    CHECK(live_univ_init(&u) == 0, "init2");
    CHECK(live_univ_reserve(&u, 4096) == 0, "reserve 4096");
    CHECK(u.cap >= 4096, "cap 4096");
    live_univ_free(&u);

    a = calloc(1, sizeof(*a));
    CHECK(a != NULL, "gen alloc");
    CHECK(universe_gen_current() == NULL, "no gen yet");
    /* publish an empty-but-built-failed gen is not allowed; publish a stub. */
    a->gen_id = 7;
    a->n_pools = 528;
    a->immutable = 1;
    prev = universe_gen_publish(a);
    CHECK(prev == NULL, "first publish");
    CHECK(universe_gen_current() == a, "hot path reads pointer");
    CHECK(universe_gen_current()->n_pools == 528, "528 visible");
    prev = universe_gen_publish(NULL);
    CHECK(prev == a, "swap out");
    free(a);

    printf("UNIV-GEN  sizeof(live_univ_t)=%zu hard_max=%u PASS\n",
           sizeof(live_univ_t), UNIV_POOL_HARD_MAX);
    return 0;
}
