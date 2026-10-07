#include "state/authvec.h"
#include "state/boot.h"
#include "state/compact.h"
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
    static pumpstate_out_t out;
    static authvec_t av;
    pump_state_t ps;
    const pump_state_t *after;
    auth_write_t w;
    uint32_t n, pid;
    uint8_t other_sig[64];

    hex32(pool, 0xab);
    hex32(vb, 0xb1);
    hex32(vq, 0xb2);
    memset(&ps, 0, sizeof(ps));
    ps.reserve_base = 1ull << 40;
    ps.reserve_quote = 1ull << 40;
    ps.virtual_quote = 7;
    ps.lp_fee_bps = 20;

    compact_state_clear(&st);
    CHECK(compact_pool_arm(&st, pool, vb, vq, &ps, 8, &pid) == 0, "arm");
    CHECK(st.pool[pid].auth_bits == POOL_AUTH_READY, "ready");
    CHECK(st.pool[pid].anchor_slot == 8, "anchor_slot");
    CHECK(st.n_token == 2, "vault tokens");

    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 9, &out) == PS_OK, "predict");
    CHECK(overlay_pump(&out.ar.overlay, pid, &after) == 0, "overlay");

    authvec_clear(&av);
    CHECK(authvec_note_pred(&av, &st, &out.tx, pid, after) == 0, "note");

    fill_w(&w, out.tx.sig, pool, 9, AUTH_ROLE_POOL);
    w.have_vq = 1;
    w.have_fees = 1;
    w.have_reserves = 1;
    w.pump = *after;
    CHECK(authvec_put_write(&av, &w) == 0, "pool write");
    fill_w(&w, out.tx.sig, vb, 9, AUTH_ROLE_VB);
    w.have_amount = 1;
    w.amount = after->reserve_base;
    CHECK(authvec_put_write(&av, &w) == 0, "vb write");
    fill_w(&w, out.tx.sig, vq, 9, AUTH_ROLE_VQ);
    w.have_amount = 1;
    w.amount = after->reserve_quote;
    CHECK(authvec_put_write(&av, &w) == 0, "vq write");
    authvec_judge(&av);
    CHECK(av.n_exact == 1 && av.pred[0].judged == AUTH_EXACT, "terminal exact");

    av.write[0].pump.reserve_base = after->reserve_base + 3;
    av.write[0].pump.reserve_quote = after->reserve_quote + 3;
    av.write[1].amount = after->reserve_base + 3;
    av.write[2].amount = after->reserve_quote + 3;
    authvec_judge(&av);
    CHECK(av.n_mismatch == 1, "mismatch");

    av.write[0].pump.reserve_base = after->reserve_base;
    av.write[0].pump.reserve_quote = after->reserve_quote;
    av.write[1].amount = after->reserve_base;
    av.write[2].amount = after->reserve_quote;
    av.write[0].pump.reserve_quote = after->reserve_quote + 5;
    authvec_judge(&av);
    CHECK(av.n_incomplete == 1 && av.n_mixed == 1, "mixed pool/vault refused");

    authvec_clear(&av);
    CHECK(authvec_note_pred(&av, &st, &out.tx, pid, after) == 0, "note2");
    fill_w(&w, out.tx.sig, pool, 9, AUTH_ROLE_POOL);
    w.have_vq = 1;
    w.have_fees = 1;
    w.pump = *after;
    w.have_reserves = 0;
    (void)authvec_put_write(&av, &w);
    fill_w(&w, out.tx.sig, vb, 9, AUTH_ROLE_VB);
    w.have_amount = 1;
    w.amount = after->reserve_base;
    (void)authvec_put_write(&av, &w);
    memset(other_sig, 0x22, 64);
    fill_w(&w, other_sig, vq, 9, AUTH_ROLE_VQ);
    w.have_amount = 1;
    w.amount = after->reserve_quote;
    (void)authvec_put_write(&av, &w);
    authvec_judge(&av);
    CHECK(av.n_incomplete == 1 && av.n_exact == 0, "foreign-sig vault ignored");

    compact_state_clear(&st);
    CHECK(boot_load_jsonl(&st, "tests/fixtures/authstate/seed.jsonl") == 0,
          "boot jsonl");
    CHECK(st.n_pool == 1 && st.pool[0].pump.virtual_quote == 7, "seeded vq");
    CHECK(st.pool[0].auth_bits == POOL_AUTH_READY, "boot ready");

    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    compact_state_clear(&st);
    CHECK(compact_pool_arm(&st, pool, vb, vq, &ps, 1, &pid) == 0, "arm2");
    CHECK(compact_pool_mark_dirty(&st, pid) == 0, "dirty");
    CHECK(pumpstate_from_bytes(&st, NULL, tx, n, 9, &out) == PS_GAP, "gap");

    if (g_fail) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nauthstate ok\n");
    return 0;
}
