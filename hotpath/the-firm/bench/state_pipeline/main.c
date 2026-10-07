#include "deps/class.h"
#include "ingress/tx.h"
#include "net/time.h"
#include "publish/publish.h"
#include "state/compact.h"
#include "transitions/overlay.h"
#include "transitions/token.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARM  1000u
#define ITERS 8000u

enum {
    ST_CLASSIFY = 0,
    ST_LOOKUP,
    ST_CLONE,
    ST_PUMP,
    ST_TOKEN,
    ST_PUBLISH,
    ST_TOTAL,
    ST_N
};

static const char *const ST_NAME[ST_N] = {
    "classify",
    "dep_lookup",
    "overlay_clone",
    "pump_exec",
    "token_update",
    "publish",
    "total",
};

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

static void
report(const char *name, uint64_t *s, uint32_t n)
{
    uint32_t i;
    uint64_t sum = 0;

    qsort(s, n, sizeof(*s), cmp_u64);
    for (i = 0; i < n; i++) {
        sum += s[i];
    }
    printf("  %-14s  min %" PRIu64 "  p50 %" PRIu64 "  p95 %" PRIu64
           "  p99 %" PRIu64 "  max %" PRIu64 "  mean %.1f ns\n",
           name, s[0], s[n / 2], s[(n * 95u) / 100u], s[(n * 99u) / 100u],
           s[n - 1], (double)sum / (double)n);
}

static void
build_tx(ordered_tx_t *tx, uint32_t pid, uint32_t src, uint32_t dst,
         uint32_t vb, uint32_t vq, uint32_t user, uint32_t wsol)
{
    ordered_tx_clear(tx);
    tx->slot = 42;
    tx->tx_index = 1;
    tx->n_ix = 3;
    tx->ix[0].proto = PROTO_SYSTEM;
    tx->ix[0].kind = IX_KIND_SYS_TRANSFER;
    tx->ix[0].relevant = 1;
    tx->ix[0].src_sys = user;
    tx->ix[0].dst_token = wsol;
    tx->ix[0].amount_in = 50;
    tx->ix[1].proto = PROTO_TOKEN;
    tx->ix[1].kind = IX_KIND_TOKEN_SYNC;
    tx->ix[1].relevant = 1;
    tx->ix[1].src_token = wsol;
    tx->ix[2].proto = PROTO_PUMP;
    tx->ix[2].kind = IX_KIND_PUMP_SELL;
    tx->ix[2].relevant = 1;
    tx->ix[2].direction = PUMP_DIR_BASE_TO_QUOTE;
    tx->ix[2].pool_id = pid;
    tx->ix[2].src_token = src;
    tx->ix[2].dst_token = dst;
    tx->ix[2].vault_base = vb;
    tx->ix[2].vault_quote = vq;
    tx->ix[2].amount_in = 10;
}

static int
seed(compact_state_t *st, uint32_t *pid, uint32_t *src, uint32_t *dst,
     uint32_t *vb, uint32_t *vq, uint32_t *user, uint32_t *wsol)
{
    pump_state_t p;
    state_token_t t;
    state_sys_t s;

    compact_state_clear(st);
    memset(&p, 0, sizeof(p));
    p.reserve_base = 10000000;
    p.reserve_quote = 10000000;
    p.lp_fee_bps = 20;
    if (compact_pool_add_pump(st, NULL, &p, pid) != 0) {
        return -1;
    }
    memset(&t, 0, sizeof(t));
    t.amount = 100000;
    t.token_program = TOKEN_PROG_SPL;
    t.mint[0] = 1;
    if (compact_token_add(st, &t, src) != 0) {
        return -1;
    }
    t.amount = 0;
    t.mint[0] = 2;
    t.flags = TOKEN_FLAG_WSOL;
    if (compact_token_add(st, &t, dst) != 0) {
        return -1;
    }
    t.flags = 0;
    t.amount = p.reserve_base;
    t.mint[0] = 1;
    if (compact_token_add(st, &t, vb) != 0) {
        return -1;
    }
    t.amount = p.reserve_quote;
    t.mint[0] = 2;
    t.flags = TOKEN_FLAG_WSOL;
    if (compact_token_add(st, &t, vq) != 0) {
        return -1;
    }
    t.amount = 0;
    t.native_reserve = 0;
    if (compact_token_add(st, &t, wsol) != 0) {
        return -1;
    }
    memset(&s, 0, sizeof(s));
    s.lamports = 1000000;
    return compact_sys_add(st, &s, user);
}

int
main(void)
{
    compact_state_t *st;
    ordered_tx_t tx;
    net_tsc_clock_t tsc;
    uint64_t *acc[ST_N];
    uint32_t i, n = 0, pid, src, dst, vb, vq, user, wsol;
    uint32_t k;

    if (net_tsc_calibrate(&tsc) != 0) {
        return 1;
    }
    st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return 1;
    }
    for (k = 0; k < ST_N; k++) {
        acc[k] = calloc(ITERS, sizeof(uint64_t));
        if (acc[k] == NULL) {
            return 1;
        }
    }
    if (seed(st, &pid, &src, &dst, &vb, &vq, &user, &wsol) != 0) {
        return 1;
    }
    build_tx(&tx, pid, src, dst, vb, vq, user, wsol);

    for (i = 0; i < WARM + ITERS; i++) {
        dep_report_t dep;
        tx_overlay_t ov;
        pump_swap_result_t res;
        const pump_state_t *after;
        uint64_t t0, t1, t2, t3, t4, t5, t6, t7, t8;

        if (seed(st, &pid, &src, &dst, &vb, &vq, &user, &wsol) != 0) {
            break;
        }
        build_tx(&tx, pid, src, dst, vb, vq, user, wsol);

        t0 = net_rdtscp();
        deps_classify(&tx, &dep);
        t1 = net_rdtscp();
        (void)deps_lookup(st, &tx, &dep);
        t2 = net_rdtscp();
        overlay_clear(&ov);
        (void)overlay_begin(&ov, st, &tx);
        t3 = net_rdtscp();
        (void)overlay_apply_token_ix(&ov, st, &tx.ix[0]);
        (void)overlay_apply_token_ix(&ov, st, &tx.ix[1]);
        t4 = net_rdtscp();
        (void)overlay_apply_pump_ix(&ov, st, &tx.ix[2], &res);
        (void)overlay_pump(&ov, pid, &after);
        t5 = net_rdtscp();
        t6 = t5;
        if (after != NULL) {
            (void)overlay_apply_pump_tokens(&ov, st, &tx.ix[2], &res, after);
        }
        t7 = net_rdtscp();
        (void)state_publish(st, &ov, &tx, &dep);
        t8 = net_rdtscp();
        if (i >= WARM && n < ITERS) {
            acc[ST_CLASSIFY][n] = net_tsc_to_ns(&tsc, t1 - t0);
            acc[ST_LOOKUP][n] = net_tsc_to_ns(&tsc, t2 - t1);
            acc[ST_CLONE][n] = net_tsc_to_ns(&tsc, t3 - t2);
            acc[ST_TOKEN][n] = net_tsc_to_ns(&tsc, (t4 - t3) + (t7 - t6));
            acc[ST_PUMP][n] = net_tsc_to_ns(&tsc, t5 - t4);
            acc[ST_PUBLISH][n] = net_tsc_to_ns(&tsc, t8 - t7);
            acc[ST_TOTAL][n] = net_tsc_to_ns(&tsc, t8 - t0);
            n++;
        }
    }

    if (n == 0) {
        return 1;
    }
    printf("STATE PIPELINE  classify + lookup + clone + pump + token + publish\n");
    printf("samples=%u  tsc_hz=%.6e\n", n, tsc.hz);
    for (k = 0; k < ST_N; k++) {
        report(ST_NAME[k], acc[k], n);
    }
    printf("\nintra-process rdtscp. Seed/reset is outside the timed window.\n");
    free(st);
    for (k = 0; k < ST_N; k++) {
        free(acc[k]);
    }
    return 0;
}
