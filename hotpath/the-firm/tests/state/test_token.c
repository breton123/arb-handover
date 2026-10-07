#include "ingress/tx.h"
#include "state/compact.h"
#include "transitions/apply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL  %s\n", (msg)); \
            g_fail++;                             \
        } else {                                  \
            printf("ok    %s\n", (msg));          \
        }                                         \
    } while (0)

static void
seed_token(state_token_t *t, uint64_t amt, uint8_t wsol)
{
    memset(t, 0, sizeof(*t));
    t->amount = amt;
    t->native_reserve = wsol ? amt : 0;
    t->token_program = TOKEN_PROG_SPL;
    t->flags = wsol ? TOKEN_FLAG_WSOL : 0;
    t->mint[0] = wsol ? 1 : 2;
    t->owner[0] = 9;
}

int
main(void)
{
    compact_state_t *st;
    ordered_tx_t tx;
    apply_result_t ar;
    state_token_t ta, tb, wsol, vault_b, vault_q;
    state_sys_t user, ata_sys;
    pump_state_t pool;
    uint32_t id_a = 0, id_b = 0, id_w = 0, id_vb = 0, id_vq = 0;
    uint32_t id_user = 0, id_ata = 0, pid = 0, reserved = 0;

    st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return 1;
    }

    seed_token(&ta, 1000, 0);
    seed_token(&tb, 0, 0);
    memcpy(tb.mint, ta.mint, 32);
    seed_token(&wsol, 0, 1);
    wsol.native_reserve = 0;
    seed_token(&vault_b, 1000000, 0);
    vault_b.mint[0] = 3;
    seed_token(&vault_q, 1000000, 1);
    memset(&pool, 0, sizeof(pool));
    pool.reserve_base = 1000000;
    pool.reserve_quote = 1000000;
    pool.lp_fee_bps = 20;
    memset(&user, 0, sizeof(user));
    user.lamports = 5000;
    memset(&ata_sys, 0, sizeof(ata_sys));

    compact_state_clear(st);
    CHECK(compact_token_add(st, &ta, &id_a) == 0, "add a");
    CHECK(compact_token_add(st, &tb, &id_b) == 0, "add b");
    CHECK(compact_token_add(st, &wsol, &id_w) == 0, "add wsol");
    CHECK(compact_token_add(st, &vault_b, &id_vb) == 0, "vault base");
    CHECK(compact_token_add(st, &vault_q, &id_vq) == 0, "vault quote");
    CHECK(compact_sys_add(st, &user, &id_user) == 0, "user sol");
    CHECK(compact_sys_add(st, &ata_sys, &id_ata) == 0, "ata sol");
    CHECK(compact_pool_add_pump(st, NULL, &pool, &pid) == 0, "pool");
    CHECK(compact_token_reserve(st, NULL, &reserved) == 0, "reserve ata");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_TOKEN;
    tx.ix[0].kind = IX_KIND_TOKEN_XFER;
    tx.ix[0].relevant = 1;
    tx.ix[0].src_token = id_a;
    tx.ix[0].dst_token = id_b;
    tx.ix[0].amount_in = 250;
    CHECK(apply_tx(st, &tx, &ar) == APPLY_OK, "transfer");
    CHECK(st->token[id_a].amount == 750 && st->token[id_b].amount == 250,
          "transfer balances");

    ordered_tx_clear(&tx);
    tx.n_ix = 2;
    tx.ix[0].proto = PROTO_SYSTEM;
    tx.ix[0].kind = IX_KIND_SYS_TRANSFER;
    tx.ix[0].relevant = 1;
    tx.ix[0].src_sys = id_user;
    tx.ix[0].dst_token = id_w;
    tx.ix[0].amount_in = 1000;
    tx.ix[1].proto = PROTO_TOKEN;
    tx.ix[1].kind = IX_KIND_TOKEN_SYNC;
    tx.ix[1].relevant = 1;
    tx.ix[1].src_token = id_w;
    CHECK(apply_tx(st, &tx, &ar) == APPLY_OK, "wrap + sync");
    CHECK(st->sys[id_user].lamports == 4000, "sol debit");
    CHECK(st->token[id_w].native_reserve == 1000 && st->token[id_w].amount == 1000,
          "wsol synced");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_ATA;
    tx.ix[0].kind = IX_KIND_ATA_CREATE;
    tx.ix[0].relevant = 1;
    tx.ix[0].dst_token = reserved;
    tx.ix[0].mint[0] = 7;
    tx.ix[0].owner[0] = 8;
    CHECK(apply_tx(st, &tx, &ar) == APPLY_OK, "ata create");
    CHECK(st->token[reserved].live && st->token[reserved].amount == 0,
          "ata live");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_PUMP;
    tx.ix[0].kind = IX_KIND_PUMP_SELL;
    tx.ix[0].relevant = 1;
    tx.ix[0].direction = PUMP_DIR_BASE_TO_QUOTE;
    tx.ix[0].pool_id = pid;
    tx.ix[0].src_token = id_a;
    tx.ix[0].dst_token = id_w;
    tx.ix[0].vault_base = id_vb;
    tx.ix[0].vault_quote = id_vq;
    tx.ix[0].amount_in = 100;
    CHECK(apply_tx(st, &tx, &ar) == APPLY_OK, "pump sell + tokens");
    CHECK(st->token[id_a].amount == 650, "user base debit");
    CHECK(st->token[id_vb].amount == st->pool[pid].pump.reserve_base,
          "base vault synced");
    CHECK(st->token[id_vq].amount == st->pool[pid].pump.reserve_quote,
          "quote vault synced");

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_TOKEN;
    tx.ix[0].kind = IX_KIND_TOKEN_CLOSE;
    tx.ix[0].relevant = 1;
    tx.ix[0].src_token = id_w;
    tx.ix[0].dst_sys = id_user;
    CHECK(apply_tx(st, &tx, &ar) == APPLY_OK, "close wsol");
    CHECK(!st->token[id_w].live, "wsol closed");
    CHECK(st->sys[id_user].lamports > 4000, "unwrap credit");

    {
        uint32_t bad = 0;
        state_token_t t22;

        seed_token(&t22, 10, 0);
        t22.token_program = TOKEN_PROG_2022;
        t22.extensions_mask = 1;
        CHECK(compact_token_add(st, &t22, &bad) == 0, "t22 row");
        ordered_tx_clear(&tx);
        tx.n_ix = 1;
        tx.ix[0].proto = PROTO_TOKEN;
        tx.ix[0].kind = IX_KIND_TOKEN_XFER;
        tx.ix[0].relevant = 1;
        tx.ix[0].src_token = bad;
        tx.ix[0].dst_token = id_b;
        tx.ix[0].amount_in = 1;
        CHECK(apply_tx(st, &tx, &ar) == APPLY_REJECT, "t22 extensions fail-closed");
        CHECK(ar.dep_class == DEP_UNKNOWN, "t22 UNKNOWN");
    }

    ordered_tx_clear(&tx);
    tx.n_ix = 1;
    tx.ix[0].proto = PROTO_TOKEN;
    tx.ix[0].kind = IX_KIND_OTHER;
    tx.ix[0].relevant = 1;
    tx.ix[0].src_token = id_a;
    CHECK(apply_tx(st, &tx, &ar) == APPLY_REJECT, "unknown token ix");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        free(st);
        return 1;
    }
    printf("\ntoken ok\n");
    free(st);
    return 0;
}
