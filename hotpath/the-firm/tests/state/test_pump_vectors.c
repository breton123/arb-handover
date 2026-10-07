#include "protocols/pump/pump.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int
quote_agrees_apply(const pump_state_t *s, uint64_t ain, uint8_t dir)
{
    pump_state_t snap = *s;
    pump_state_t after;
    pump_swap_ix_t ix;
    pump_swap_result_t res;
    pump_quote_t q;
    int arc, qrc;

    ix.amount_in = ain;
    ix.min_amount_out = 0;
    ix.direction = dir;
    arc = pump_apply_swap(s, &ix, &after, &res);
    qrc = pump_quote_exact_in(s, ain, dir, &q);
    if (memcmp(s, &snap, sizeof(snap)) != 0) {
        fprintf(stderr, "quote mutated S\n");
        return -1;
    }
    if (arc != 0) {
        return (qrc != 0 && !q.valid) ? 0 : -1;
    }
    if (qrc != 0 || !q.valid || q.amount_out != res.amount_out
        || q.fee != res.fee) {
        return -1;
    }
    return 0;
}

static FILE *
open_vectors(const char *path)
{
    static const char *const fallback[] = {
        "tests/fixtures/pump/pump-vectors.txt",
        "../tests/fixtures/pump/pump-vectors.txt",
        "pump-vectors.txt",
    };
    FILE *f = NULL;
    size_t i;

    if (path != NULL) {
        f = fopen(path, "r");
    }
    for (i = 0; f == NULL && i < 3; i++) {
        f = fopen(fallback[i], "r");
    }
    return f;
}

int
main(int argc, char **argv)
{
    FILE *f;
    char line[256];
    uint32_t n = 0;
    const char *path = NULL;

    if (argc >= 3 && strcmp(argv[1], "--vectors") == 0) {
        path = argv[2];
    }
    f = open_vectors(path);
    if (f == NULL) {
        fprintf(stderr, "pump vectors not found\n");
        return 1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long ain, base, quote, virt, lp, proto, cr, sell, expect;
        pump_state_t s;
        pump_quote_t q;
        uint8_t dir;

        if (line[0] < '0' || line[0] > '9') {
            continue;
        }
        if (sscanf(line, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &ain, &base, &quote, &virt, &lp, &proto, &cr, &sell,
                   &expect) != 9) {
            fprintf(stderr, "bad vector line %u\n", n);
            fclose(f);
            return 1;
        }
        memset(&s, 0, sizeof(s));
        s.reserve_base = (uint64_t)base;
        s.reserve_quote = (uint64_t)quote;
        s.virtual_quote = (int64_t)virt;
        s.lp_fee_bps = (uint64_t)lp;
        s.protocol_fee_bps = (uint64_t)proto;
        s.creator_fee_bps = (uint64_t)cr;
        dir = sell ? PUMP_DIR_BASE_TO_QUOTE : PUMP_DIR_QUOTE_TO_BASE;
        if (pump_quote_exact_in(&s, (uint64_t)ain, dir, &q) != 0 || !q.valid) {
            fprintf(stderr, "vector %u quote fail\n", n);
            fclose(f);
            return 1;
        }
        if (q.amount_out != (uint64_t)expect) {
            fprintf(stderr, "vector %u out %" PRIu64 " != %llu\n", n,
                    q.amount_out, expect);
            fclose(f);
            return 1;
        }
        if (quote_agrees_apply(&s, (uint64_t)ain, dir) != 0) {
            fprintf(stderr, "vector %u quote!=apply\n", n);
            fclose(f);
            return 1;
        }
        n++;
    }
    fclose(f);
    if (n != 10000) {
        fprintf(stderr, "expected 10000 vectors, got %u\n", n);
        return 1;
    }
    printf("ok    official SDK vectors %u / %u bit-exact amount_out\n", n, n);
    printf("\npump_vectors ok\n");
    return 0;
}
