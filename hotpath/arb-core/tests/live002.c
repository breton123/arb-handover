#include "live.h"
#include "paper.h"

#include <inttypes.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
    live_univ_t u;
    univ_state_t st;
    paper_opp_t opp;
    const char *path;

    if (argc != 2) {
        fprintf(stderr, "usage: %s liveuniv.bin\n", argv[0]);
        return 1;
    }
    path = argv[1];
    if (live_univ_init(&u) != 0 || live_univ_load(&u, path) != 0) {
        fprintf(stderr, "FAIL  load\n");
        return 1;
    }
    if (univ_state_init(&st, u.n) != 0 || live_fill_univ_state(&u, &st) != 0) {
        fprintf(stderr, "FAIL  state\n");
        return 1;
    }
    printf("LIVE-002  ver=%u pools=%u routes=%u  dlmm=%u pump=%u clmm=%u cpmm=%u damm=%u orca=%u\n",
           u.file_ver, u.n, u.routes.n_route, u.n_dlmm, u.n_pump,
           u.n_clmm, u.n_cpmm, u.n_damm, u.n_orca);
    if (paper_eval_pool(&u, &st, 0, &opp) != 0) {
        fprintf(stderr, "FAIL  eval\n");
        return 1;
    }
    printf("  pool0 searchable=%u signed_ready=%u reason=%u valid=%u gp=%" PRIu64 "\n",
           opp.searchable, opp.signed_ready, opp.reason, opp.opp.valid,
           opp.opp.gross_profit);
    univ_state_free(&st);
    live_univ_free(&u);
    return 0;
}
