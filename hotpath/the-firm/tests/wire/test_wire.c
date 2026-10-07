#include "deps/class.h"
#include "state/compact.h"
#include "wire/mkpkt.h"
#include "wire/pipeline.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(c, m)                       \
    do {                                  \
        if (!(c)) {                       \
            fprintf(stderr, "FAIL  %s\n", (m)); \
            g_fail++;                     \
        } else {                          \
            printf("ok    %s\n", (m));    \
        }                                 \
    } while (0)

static void
seed(compact_state_t *st, const uint8_t pool[32], uint8_t auth)
{
    pump_state_t p;
    uint32_t id = 0;

    compact_state_clear(st);
    memset(&p, 0, sizeof(p));
    p.reserve_base = 1000000;
    p.reserve_quote = 1000000;
    p.virtual_quote = 0;
    p.lp_fee_bps = 20;
    (void)compact_pool_add_pump(st, pool, &p, &id);
    st->pool[id].auth_bits = auth;
}

int
main(void)
{
    uint8_t raw[256], pkt[400];
    uint8_t pool[32];
    compact_state_t st;
    wire_in_t in;
    wire_out_t out;
    uint32_t n;
    uint16_t plen;

    memset(pool, 0xcd, 32);
    n = wire_mk_pump_sell(raw, sizeof(raw), pool, 100);
    plen = wire_wrap_shred(pkt, sizeof(pkt), 77, raw, n);
    CHECK(plen > 88, "shred wrap");

    seed(&st, pool, POOL_AUTH_READY);
    memset(&in, 0, sizeof(in));
    in.pkt = pkt;
    in.pkt_len = plen;
    in.st = &st;
    CHECK(wire_to_state(&in, &out, 3e9) == 0, "wire run");
    CHECK(out.fail == WIRE_FAIL_NONE, "complete AUTH exact");
    CHECK(out.admit == DEP_FAST_CUSTOM, "FAST_CUSTOM");
    CHECK(out.apply_rc == APPLY_OK, "published");
    CHECK(st.pool[0].pump.reserve_base == 1000100, "base +ain");

    seed(&st, pool, (uint8_t)(POOL_AUTH_READY & ~POOL_AUTH_VQ));
    CHECK(wire_to_state(&in, &out, 3e9) == 0, "wire missing V");
    CHECK(out.fail == WIRE_FAIL_PRE_VQ, "pre_virtual");
    CHECK(out.admit == DEP_FALLBACK, "fallback not exact");
    CHECK(st.pool[0].pump.reserve_base == 1000000, "no publish");

    pkt[85] = 0;
    seed(&st, pool, POOL_AUTH_READY);
    CHECK(wire_to_state(&in, &out, 3e9) == 0, "incomplete shred");
    CHECK(out.fail == WIRE_FAIL_TX_WAIT, "tx wait");

    if (g_fail) {
        return 1;
    }
    printf("\nwire ok\n");
    return 0;
}
