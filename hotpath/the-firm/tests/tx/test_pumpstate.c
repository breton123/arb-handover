#include "ingress/resolve.h"
#include "state/compact.h"
#include "transitions/overlay.h"
#include "tx/bind.h"
#include "tx/pumpstate.h"
#include "tx/resolve.h"
#include "tx/view.h"
#include "wire/mkpkt.h"

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

static uint32_t
sv(uint8_t *p, uint32_t v)
{
    uint32_t n = 0;

    do {
        uint8_t b = (uint8_t)(v & 0x7fu);

        v >>= 7;
        if (v) {
            b |= 0x80u;
        }
        p[n++] = b;
    } while (v);
    return n;
}

/* static[payer, program] + writable LUT pool + readonly extra */
static uint32_t
mk_v0_lut(uint8_t *buf, const uint8_t pool[32], const uint8_t ta[32],
          const uint8_t tb[32], const uint8_t extra[32], uint64_t ain)
{
    uint8_t *p = buf;
    uint8_t payer[32];

    memset(payer, 0x01, 32);
    p += sv(p, 1);
    memset(p, 0x11, 64);
    p += 64;
    *p++ = 0x80;
    *p++ = 1;
    *p++ = 0;
    *p++ = 1;
    p += sv(p, 2);
    memcpy(p, payer, 32);
    p += 32;
    memcpy(p, WIRE_PUMP_PK, 32);
    p += 32;
    memset(p, 0x22, 32);
    p += 32;
    p += sv(p, 1);
    *p++ = 1;
    p += sv(p, 1);
    *p++ = 2; /* first loaded writable = pool */
    p += sv(p, 24);
    memcpy(p, WIRE_SELL, 8);
    p += 8;
    memcpy(p, &ain, 8);
    p += 8;
    memset(p, 0, 8);
    p += 8;
    p += sv(p, 2);
    memcpy(p, ta, 32);
    p += 32;
    p += sv(p, 1);
    *p++ = 0;
    p += sv(p, 0);
    memcpy(p, tb, 32);
    p += 32;
    p += sv(p, 0);
    p += sv(p, 1);
    *p++ = 0;
    (void)pool;
    (void)extra;
    return (uint32_t)(p - buf);
}

int
main(void)
{
    uint8_t tx[512], pool[32], ta[32], tb[32], extra[32];
    static compact_state_t st;
    static pumpstate_out_t out;
    static ordered_tx_t ot;
    static wire_lut_tab_t luts;
    static tx_view_t v;
    pump_state_t ps;
    uint32_t n, pid;

    memset(pool, 0xab, 32);
    memset(ta, 0xa1, 32);
    memset(tb, 0xb2, 32);
    memset(extra, 0xee, 32);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    CHECK(txview_decode(tx, n, &v) == TXV_OK, "legacy decode");
    CHECK(v.version == 0 && v.n_ix == 1 && v.n_alt == 0, "legacy shape");
    CHECK(v.ix[0].dlen == 24 && v.ix[0].data[0] == 0x33, "raw ix data");
    CHECK(txview_resolve(&v, NULL) == TXV_RES_OK, "no alt");
    CHECK(txview_bind(&v, 7, &ot) == 0, "bind");
    CHECK(ot.ix[0].proto == PROTO_PUMP && ot.ix[0].kind == IX_KIND_PUMP_SELL,
          "pump sell");
    CHECK(ot.ix[0].amount_in == 1000, "amount");

    n = mk_v0_lut(tx, pool, ta, tb, extra, 50);
    CHECK(txview_decode(tx, n, &v) == TXV_OK, "v0 decode");
    CHECK(v.version == 1 && v.n_alt == 2 && v.n_static == 2, "v0 alts");
    CHECK(txview_resolve(&v, NULL) == TXV_RES_LUT, "missing lut fail closed");

    wire_lut_tab_clear(&luts);
    CHECK(wire_lut_put(&luts, ta, pool, 1) == 0, "lut A");
    CHECK(wire_lut_put(&luts, tb, extra, 1) == 0, "lut B");
    CHECK(txview_decode(tx, n, &v) == TXV_OK, "v0 again");
    CHECK(txview_resolve(&v, &luts) == TXV_RES_OK, "canonical resolve");
    CHECK(v.n_keys == 4, "static+w+r");
    CHECK(memcmp(v.key[2], pool, 32) == 0, "writables before readables");
    CHECK(memcmp(v.key[3], extra, 32) == 0, "readonly last");
    CHECK(txview_bind(&v, 8, &ot) == 0, "bind v0");
    CHECK(ot.ix[0].proto == PROTO_PUMP, "program still static");
    CHECK(ot.acc_ix[0][0] == 2, "pool via loaded writable index");

    memset(&ps, 0, sizeof(ps));
    ps.reserve_base = 1ull << 40;
    ps.reserve_quote = 1ull << 40;
    ps.lp_fee_bps = 20;
    compact_state_clear(&st);
    CHECK(compact_pool_add_pump(&st, pool, &ps, &pid) == 0, "bootstrap");
    st.pool[pid].auth_bits = POOL_AUTH_READY;
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 9, &out) == PS_OK,
          "exact apply");
    CHECK(out.predicted == 1 && out.n_pump == 1, "predicted");
    CHECK(st.pool[pid].grade == POOL_GRADE_EXACT, "still exact");
    {
        const pump_state_t *after;

        CHECK(overlay_pump(&out.ar.overlay, pid, &after) == 0, "overlay row");
        CHECK(after->reserve_base > ps.reserve_base, "sell raises base");
    }

    tx[n - 24] = 0x00;
    CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 9, &out) == PS_UNSUPPORTED,
          "unknown disc dirty");
    CHECK(st.pool[pid].grade == POOL_GRADE_DIRTY, "dirtied");

    if (g_fail) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\npumpstate ok\n");
    return 0;
}
