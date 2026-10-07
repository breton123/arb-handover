#include "state/compact.h"
#include "tx/pumpstate.h"
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

int
main(void)
{
    uint8_t tx[512], pool[32], vb[32], vq[32];
    static compact_state_t st;
    static pumpstate_out_t out;
    pump_state_t ps;
    uint32_t n, pid;
    uint64_t rb0;

    memset(pool, 0xab, 32);
    memset(vb, 0xb1, 32);
    memset(vq, 0xb2, 32);
    memset(&ps, 0, sizeof(ps));
    ps.reserve_base = 1ull << 40;
    ps.reserve_quote = 1ull << 40;
    ps.virtual_quote = 7;
    ps.lp_fee_bps = 20;

    compact_state_clear(&st);
    CHECK(compact_pool_arm(&st, pool, vb, vq, &ps, 10, &pid) == 0, "arm S=10");
    CHECK(st.pool[pid].anchor_slot == 10, "per-pool anchor");
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    rb0 = st.pool[pid].pump.reserve_base;
    CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 10, &out) == PS_BEFORE_ANCHOR,
          "slot S discarded");
    CHECK(st.pool[pid].pump.reserve_base == rb0, "no apply at S");
    CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 11, &out) == PS_OK,
          "slot S+1 apply");
    CHECK(st.pool[pid].pump.reserve_base > rb0, "WAL advanced");

    CHECK(compact_pool_mark_dirty(&st, pid) == 0, "dirty");
    CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 12, &out) == PS_GAP, "gap");
    ps.reserve_base = 1ull << 40;
    ps.reserve_quote = 1ull << 40;
    ps.virtual_quote = 9;
    CHECK(compact_pool_arm(&st, pool, vb, vq, &ps, 12, &pid) == 0, "re-anchor");
    CHECK(st.pool[pid].grade == POOL_GRADE_EXACT, "exact again");
    CHECK(st.pool[pid].pump.virtual_quote == 9, "refreshed vq");
    CHECK(st.pool[pid].anchor_slot == 12, "new per-pool anchor");

    {
        uint8_t pool_b[32], vb_b[32], vq_b[32];
        uint32_t pid_b;
        uint8_t txb[512];
        uint32_t nb;
        pump_state_t psb = ps;

        memset(pool_b, 0xac, 32);
        memset(vb_b, 0xc1, 32);
        memset(vq_b, 0xc2, 32);
        CHECK(compact_pool_arm(&st, pool_b, vb_b, vq_b, &psb, 20, &pid_b) == 0,
              "arm B@20");
        nb = wire_mk_pump_sell(txb, sizeof(txb), pool_b, 1000);
        CHECK(pumpstate_from_bytes(&st, NULL, txb, nb, 15, &out)
                  == PS_BEFORE_ANCHOR,
              "B ignores tx at 15");
        CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 15, &out) == PS_OK,
              "A still applies at 15");
    }

    if (g_fail) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\ncatchup ok\n");
    return 0;
}
