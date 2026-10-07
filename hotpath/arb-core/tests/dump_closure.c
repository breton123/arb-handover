/* Dump compiled {DLMM,Pump} closure from liveuniv.bin. */
#include "live.h"
#include "proto.h"
#include "universe.h"

#include <stdio.h>
#include <string.h>

static const char *
pname(uint8_t p)
{
    switch (p) {
    case PROTO_DLMM: return "dlmm";
    case PROTO_PUMP: return "pump";
    case PROTO_CLMM: return "clmm";
    case PROTO_CPMM: return "cpmm";
    case PROTO_DAMM: return "damm";
    case PROTO_ORCA: return "orca";
    default: return "?";
    }
}

int
main(int argc, char **argv)
{
    live_univ_t u;
    uint32_t i, exec_n = 0, miss = 0, fam255 = 0;
    uint32_t fam[256];
    const char *path;

    memset(fam, 0, sizeof(fam));
    path = (argc == 2) ? argv[1] : "/home/louis/captures/paper_orbit/liveuniv.bin";
    if (live_univ_init(&u) != 0 || live_univ_load(&u, path) != 0) {
        fprintf(stderr, "FAIL load %s\n", path);
        return 1;
    }
    printf("{\n  \"n_pool\": %u,\n  \"n_route\": %u,\n  \"routes\": [\n",
           u.n, u.routes.n_route);
    for (i = 0; i < u.routes.n_route; i++) {
        const compiled_route_t *r = &u.routes.route[i];
        char seq[64];
        int n;
        n = snprintf(seq, sizeof(seq), "%s%s%s%s%s",
                     r->n_hop >= 1 ? pname(r->proto[0]) : "",
                     r->n_hop >= 2 ? "-" : "",
                     r->n_hop >= 2 ? pname(r->proto[1]) : "",
                     r->n_hop >= 3 ? "-" : "",
                     r->n_hop >= 3 ? pname(r->proto[2]) : "");
        (void)n;
        fam[r->family]++;
        if (route_executable(r)) {
            exec_n++;
        } else {
            miss++;
        }
        if (r->family == ROUTE_FAM_OTHER) {
            fam255++;
        }
        printf("    {\"id\":%u,\"fam\":%u,\"exec\":%u,\"n_hop\":%u,"
               "\"seq\":\"%s\",\"p0\":%u,\"p1\":%u,\"p2\":%u}%s\n",
               i, r->family, route_executable(r) ? 1u : 0u, r->n_hop, seq,
               r->n_hop >= 1 ? r->pool[0] : 0u,
               r->n_hop >= 2 ? r->pool[1] : 0u,
               r->n_hop >= 3 ? r->pool[2] : 0u,
               (i + 1 < u.routes.n_route) ? "," : "");
    }
    printf("  ],\n  \"executable\": %u,\n  \"exec_missing\": %u,\n"
           "  \"family255\": %u,\n  \"fam0\": %u,\n  \"fam5\": %u,\n"
           "  \"fam6\": %u\n}\n",
           exec_n, miss, fam255, fam[0], fam[5], fam[6]);
    live_univ_free(&u);
    return 0;
}
