/*
 * Measure DLMM walk distance on a fat (control-plane) bin dump.
 * Does not change DLMM_CACHE_K. Does not send.
 *
 * events:  pool_idx amount_in dir
 * bins:    optional dir/pool_<idx>.bins  (active bin_step nbin / id x y)
 */
#include "dlmm.h"
#include "dlmm_cache.h"
#include "hot.h"
#include "live.h"
#include "proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static int
load_bins(const char *dir, uint32_t idx, const dlmm_pool_hot_t *h, dlmm_state_t *s)
{
    char path[512];
    FILE *f;
    int32_t active;
    unsigned bin_step, nbin, i;

    snprintf(path, sizeof(path), "%s/pool_%u.bins", dir, idx);
    f = fopen(path, "r");
    memset(s, 0, sizeof(*s));
    if (h != NULL) {
        s->parameters = h->parameters;
        s->v_parameters = h->v_parameters;
        s->status = h->status;
        s->now_ts = h->now_ts;
    }
    if (f == NULL) {
        if (h == NULL) {
            return -1;
        }
        dlmm_hot_to_state(h, s);
        return 0;
    }
    if (fscanf(f, "%d %u %u", &active, &bin_step, &nbin) != 3) {
        fclose(f);
        return -1;
    }
    s->active_id = active;
    s->bin_step = (uint16_t)bin_step;
    s->nbin = 0;
    for (i = 0; i < nbin && s->nbin < DLMM_BINS_MAX; i++) {
        int32_t id;
        unsigned long long x, y;
        if (fscanf(f, "%d %llu %llu", &id, &x, &y) != 3) {
            break;
        }
        s->bins[s->nbin].id = id;
        s->bins[s->nbin].amount_x = (uint64_t)x;
        s->bins[s->nbin].amount_y = (uint64_t)y;
        s->reserve_x += (uint64_t)x;
        s->reserve_y += (uint64_t)y;
        s->nbin++;
    }
    fclose(f);
    return s->nbin > 0 ? 0 : -1;
}

int
main(int argc, char **argv)
{
    live_univ_t u;
    FILE *ef;
    const char *univ;
    const char *events;
    const char *bindir;
    int32_t walks[1 << 16];
    uint32_t n_w = 0, n_fail = 0, n_ok = 0;
    int32_t mx = 0;
    char line[256];

    if (argc < 4) {
        fprintf(stderr, "usage: %s liveuniv.bin events.txt bins_dir\n", argv[0]);
        return 1;
    }
    univ = argv[1];
    events = argv[2];
    bindir = argv[3];
    if (live_univ_init(&u) != 0 || live_univ_load(&u, univ) != 0) {
        fprintf(stderr, "univ load failed\n");
        return 1;
    }
    ef = fopen(events, "r");
    if (ef == NULL) {
        fprintf(stderr, "events open failed\n");
        return 1;
    }
    while (fgets(line, sizeof(line), ef) != NULL && n_w < (1u << 16)) {
        unsigned idx, dir;
        unsigned long long ain;
        dlmm_state_t s, after;
        dlmm_apply_result_t res;
        hot_trigger_t n;
        const dlmm_pool_hot_t *h;
        int32_t w;

        if (sscanf(line, "%u %llu %u", &idx, &ain, &dir) != 3) {
            continue;
        }
        if (idx >= u.n || u.meta[idx].protocol != PROTO_DLMM) {
            continue;
        }
        h = dlmm_cache_get(&u.dlmm, idx);
        if (load_bins(bindir, idx, h, &s) != 0) {
            n_fail++;
            continue;
        }
        memset(&n, 0, sizeof(n));
        n.protocol = PROTO_DLMM;
        n.dlmm.amount_in = (uint64_t)ain;
        n.dlmm.min_amount_out = 0;
        n.dlmm.swap_for_y = (uint8_t)dir;
        if (dlmm_apply_swap(&s, &n.dlmm, &after, &res) != 0) {
            n_fail++;
            continue;
        }
        w = after.active_id - s.active_id;
        if (w < 0) {
            w = -w;
        }
        walks[n_w++] = w;
        n_ok++;
        if (w > mx) {
            mx = w;
        }
    }
    fclose(ef);
    if (n_w > 0) {
        qsort(walks, n_w, sizeof(*walks), cmp_i32);
    }
    printf("STATE-003 WALK  n_ok=%u n_fail=%u\n", n_ok, n_fail);
    if (n_w > 0) {
        printf("bins_required  p50=%d p90=%d p99=%d p99.9=%d max=%d\n",
               walks[(n_w - 1) * 50 / 100],
               walks[(n_w - 1) * 90 / 100],
               walks[(n_w - 1) * 99 / 100],
               walks[(n_w - 1) * 999 / 1000],
               mx);
    }
    live_univ_free(&u);
    return 0;
}
