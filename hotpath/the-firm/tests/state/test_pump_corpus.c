#include "protocols/pump/pump.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static FILE *
open_vectors(void)
{
    static const char *const paths[] = {
        "tests/fixtures/pump/pump-vectors.txt",
        "../tests/fixtures/pump/pump-vectors.txt",
    };
    FILE *f = NULL;
    size_t i;

    for (i = 0; i < 2 && f == NULL; i++) {
        f = fopen(paths[i], "r");
    }
    return f;
}

static void
fill_state(pump_state_t *s, unsigned long long base, unsigned long long quote,
           unsigned long long virt, unsigned long long lp,
           unsigned long long proto, unsigned long long cr)
{
    memset(s, 0, sizeof(*s));
    s->reserve_base = (uint64_t)base;
    s->reserve_quote = (uint64_t)quote;
    s->virtual_quote = (int64_t)virt;
    s->lp_fee_bps = (uint64_t)lp;
    s->protocol_fee_bps = (uint64_t)proto;
    s->creator_fee_bps = (uint64_t)cr;
}

int
main(void)
{
    FILE *f;
    char line[256];
    uint32_t n_in = 0, n_out = 0, n_seq = 0, n_fail_out = 0;

    /* PUMP-TX-016 two-leg: sell + buy_exact_out composition */
    {
        pump_state_t s, a, b;
        pump_swap_ix_t ix;
        pump_swap_result_t res;
        uint64_t sell_n = 11411288817ull;
        uint64_t buy_out = 11345137575ull;

        memset(&s, 0, sizeof(s));
        s.reserve_base = 1000000000000ull;
        s.reserve_quote = 1000000000000000ull;
        s.lp_fee_bps = 20;
        s.protocol_fee_bps = 5;
        s.creator_fee_bps = 5;
        ix.amount_in = sell_n;
        ix.min_amount_out = 0;
        ix.direction = PUMP_DIR_BASE_TO_QUOTE;
        if (pump_apply_swap(&s, &ix, &a, &res) != 0) {
            fprintf(stderr, "016 sell fail\n");
            return 1;
        }
        if (pump_apply_buy_exact_out(&a, buy_out, 0, &b, &res) != 0) {
            fprintf(stderr, "016 buy_out fail\n");
            return 1;
        }
        if (b.reserve_base - s.reserve_base != sell_n - buy_out) {
            fprintf(stderr, "016 composition mismatch\n");
            return 1;
        }
        printf("ok    overlay-016 sell+buy_out composition\n");
    }

    f = open_vectors();
    if (f == NULL) {
        fprintf(stderr, "vectors not found\n");
        return 1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long ain, base, quote, virt, lp, proto, cr, sell, expect;
        pump_state_t s, after, mid;
        pump_swap_ix_t ix;
        pump_swap_result_t res;
        pump_quote_t q;
        uint8_t dir;
        uint64_t bout;

        if (line[0] < '0' || line[0] > '9') {
            continue;
        }
        if (sscanf(line, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &ain, &base, &quote, &virt, &lp, &proto, &cr, &sell,
                   &expect) != 9) {
            fclose(f);
            return 1;
        }
        fill_state(&s, base, quote, virt, lp, proto, cr);
        dir = sell ? PUMP_DIR_BASE_TO_QUOTE : PUMP_DIR_QUOTE_TO_BASE;
        if (pump_quote_exact_in(&s, (uint64_t)ain, dir, &q) != 0
            || q.amount_out != (uint64_t)expect) {
            fprintf(stderr, "exact-in vector %u fail\n", n_in);
            fclose(f);
            return 1;
        }
        n_in++;

        bout = (s.reserve_base / 4u);
        if (bout > 1000u) {
            bout = 1000u;
        }
        if (bout > 0 && pump_apply_buy_exact_out(&s, bout, 0, &after, &res) == 0) {
            if (after.reserve_base != s.reserve_base - bout
                || res.amount_out != bout) {
                fprintf(stderr, "buy_out vector %u mismatch\n", n_out);
                fclose(f);
                return 1;
            }
            n_out++;
        } else if (bout > 0) {
            n_fail_out++;
        }

        if (sell && ain > 200) {
            ix.amount_in = (uint64_t)ain;
            ix.min_amount_out = 0;
            ix.direction = PUMP_DIR_BASE_TO_QUOTE;
            if (pump_apply_swap(&s, &ix, &mid, &res) == 0) {
                uint64_t back = (uint64_t)ain / 2u;
                if (back > 0
                    && pump_apply_buy_exact_out(&mid, back, 0, &after, &res)
                        == 0) {
                    if (after.reserve_base != s.reserve_base + (uint64_t)ain
                            - back) {
                        fprintf(stderr, "seq vector %u mismatch\n", n_seq);
                        fclose(f);
                        return 1;
                    }
                    n_seq++;
                }
            }
        }
    }
    fclose(f);
    if (n_in != 10000) {
        fprintf(stderr, "expected 10000 exact-in, got %u\n", n_in);
        return 1;
    }
    if (n_out < 100) {
        fprintf(stderr, "too few exact-out hits: %u\n", n_out);
        return 1;
    }
    printf("ok    exact-in corpus %u\n", n_in);
    printf("ok    exact-out invert %u (uninvertible %u)\n", n_out, n_fail_out);
    printf("ok    sell+buy_out sequences %u\n", n_seq);
    printf("\npump_corpus ok\n");
    return 0;
}
