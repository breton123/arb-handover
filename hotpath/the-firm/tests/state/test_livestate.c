#include "state/authvec.h"
#include "state/compact.h"
#include "state/live.h"
#include "wire/lut_cache.h"
#include "transitions/overlay.h"
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

static void
hex32(uint8_t *d, uint8_t v)
{
    memset(d, v, 32);
}

static void
fill_w(auth_write_t *w, const uint8_t sig[64], const uint8_t pk[32],
       uint64_t slot, uint8_t role)
{
    memset(w, 0, sizeof(*w));
    memcpy(w->sig, sig, 64);
    memcpy(w->pubkey, pk, 32);
    w->slot = slot;
    w->write_version = 1;
    w->role = role;
}

int
main(void)
{
    uint8_t tx[512], pool[32], vb[32], vq[32];
    static compact_state_t st;
    static lut_cache_t luts;
    static authvec_t av;
    static live_t L;
    pump_state_t ps;
    auth_write_t w;
    uint32_t n, pid;
    const pump_state_t *after;
    FILE *dash;

    hex32(pool, 0xab);
    hex32(vb, 0xb1);
    hex32(vq, 0xb2);
    memset(&ps, 0, sizeof(ps));
    ps.reserve_base = 1ull << 40;
    ps.reserve_quote = 1ull << 40;
    ps.virtual_quote = 7;
    ps.lp_fee_bps = 20;

    compact_state_clear(&st);
    lut_cache_clear(&luts);
    authvec_clear(&av);
    live_clear(&L, &st, &luts, &av);
    CHECK(compact_pool_arm(&st, pool, vb, vq, &ps, 8, &pid) == 0, "arm");
    {
        char buf[2048];
        FILE *pslog = tmpfile();

        CHECK(pslog != NULL, "ps tmp");
        L.ps_log = pslog;
        n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
        CHECK(live_on_framed(&L, tx, n, 9, 1000) == PS_OK, "apply");
        CHECK(L.n_applied == 1 && L.n_pump == 1, "applied pump");
        if (pslog != NULL) {
            rewind(pslog);
            CHECK(fgets(buf, sizeof(buf), pslog) != NULL
                  && strstr(buf, "\"rc\":\"OK\"") != NULL, "ps log OK");
            L.ps_log = NULL;
            fclose(pslog);
        }
    }
    CHECK(L.n_caught_up == 1, "caught up");
    CHECK(overlay_pump(&L.out.ar.overlay, pid, &after) == 0, "after");

    fill_w(&w, L.out.tx.sig, pool, 9, AUTH_ROLE_POOL);
    w.have_vq = 1;
    w.have_fees = 1;
    w.have_reserves = 1;
    w.have_disabled = 1;
    w.pump = *after;
    CHECK(authvec_put_write(&av, &w) == 0, "pool AUTH");
    fill_w(&w, L.out.tx.sig, vb, 9, AUTH_ROLE_VB);
    w.have_amount = 1;
    w.amount = after->reserve_base;
    CHECK(authvec_put_write(&av, &w) == 0, "vb AUTH");
    fill_w(&w, L.out.tx.sig, vq, 9, AUTH_ROLE_VQ);
    w.have_amount = 1;
    w.amount = after->reserve_quote;
    CHECK(authvec_put_write(&av, &w) == 0, "vq AUTH");
    live_judge(&L);
    CHECK(av.n_exact == 1 && av.n_mismatch == 0, "MATCH");
    CHECK(st.pool[pid].grade == POOL_GRADE_EXACT, "AUTH did not dirty");
    CHECK(L.av->pred[0].prestate.reserve_base == (1ull << 40), "prestate");

    /* incomplete AUTH is not a match */
    compact_state_clear(&st);
    authvec_clear(&av);
    live_clear(&L, &st, &luts, &av);
    CHECK(compact_pool_arm(&st, pool, vb, vq, &ps, 8, &pid) == 0, "arm2");
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    CHECK(live_on_framed(&L, tx, n, 9, 0) == PS_OK, "apply2");
    live_judge(&L);
    CHECK(av.n_incomplete == 1 && av.n_exact == 0, "AUTH_INCOMPLETE");
    CHECK(st.pool[pid].grade == POOL_GRADE_EXACT, "incomplete not dirty");

    /* mismatch: DIRTY + dump */
    compact_state_clear(&st);
    authvec_clear(&av);
    live_clear(&L, &st, &luts, &av);
    {
        int op = live_open_logs(&L, NULL, "build/livestate_mismatch.txt");

        if (op != 0) {
            op = live_open_logs(&L, NULL, "livestate_mismatch.txt");
        }
        CHECK(op == 0, "dump open");
    }
    CHECK(compact_pool_arm(&st, pool, vb, vq, &ps, 8, &pid) == 0, "arm3");
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    CHECK(live_on_framed(&L, tx, n, 9, 0) == PS_OK, "apply3");
    CHECK(overlay_pump(&L.out.ar.overlay, pid, &after) == 0, "after3");
    fill_w(&w, L.out.tx.sig, pool, 9, AUTH_ROLE_POOL);
    w.have_vq = 1;
    w.have_fees = 1;
    w.have_reserves = 1;
    w.pump = *after;
    w.pump.reserve_base += 1;
    w.pump.reserve_quote += 1;
    (void)authvec_put_write(&av, &w);
    fill_w(&w, L.out.tx.sig, vb, 9, AUTH_ROLE_VB);
    w.have_amount = 1;
    w.amount = after->reserve_base + 1;
    (void)authvec_put_write(&av, &w);
    fill_w(&w, L.out.tx.sig, vq, 9, AUTH_ROLE_VQ);
    w.have_amount = 1;
    w.amount = after->reserve_quote + 1;
    (void)authvec_put_write(&av, &w);
    live_judge(&L);
    CHECK(av.n_mismatch == 1, "MISMATCH");
    CHECK(st.pool[pid].grade == POOL_GRADE_DIRTY, "brutal DIRTY");
    CHECK(L.n_mismatch_dirty == 1, "dumped");
    live_close_logs(&L);

    /* NO_PRESTATE */
    compact_state_clear(&st);
    authvec_clear(&av);
    live_clear(&L, &st, &luts, &av);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    CHECK(live_on_framed(&L, tx, n, 9, 0) == PS_NO_PRESTATE, "NO_PRESTATE");
    CHECK(L.reason[PS_NO_PRESTATE] == 1, "NO_PRESTATE count");

    dash = tmpfile();
    if (dash != NULL) {
        live_print(&L, dash);
        fclose(dash);
        CHECK(1, "dashboard");
    } else {
        CHECK(1, "dashboard skip");
    }

    if (g_fail) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nlivestate ok\n");
    return 0;
}
