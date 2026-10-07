#include "oracle/soak.h"

#include "ingress/tx.h"
#include "oracle/case.h"
#include "oracle/soak_row.h"
#include "protocols/pump/pump.h"
#include "state/compact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
oracle_soak_stats_clear(oracle_soak_stats_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

static int
j_u64(const char *line, const char *key, uint64_t *out)
{
    char pat[48];
    const char *p;
    unsigned long long v;

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(line, pat);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pat);
    while (*p == ' ') {
        p++;
    }
    if (*p == '"') {
        return -1;
    }
    if (sscanf(p, "%llu", &v) != 1) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int
j_i64(const char *line, const char *key, int64_t *out)
{
    uint64_t u;
    const char *p;
    char pat[48];

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(line, pat);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pat);
    while (*p == ' ') {
        p++;
    }
    if (*p == '-') {
        long long v;
        if (sscanf(p, "%lld", &v) != 1) {
            return -1;
        }
        *out = (int64_t)v;
        return 0;
    }
    if (j_u64(line, key, &u) != 0) {
        return -1;
    }
    *out = (int64_t)u;
    return 0;
}

static int
j_str(const char *line, const char *key, char *out, size_t cap)
{
    char pat[48];
    const char *p;
    size_t n = 0;

    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(line, pat);
    if (p == NULL) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    p += strlen(pat);
    while (*p != '\0' && *p != '"' && n + 1 < cap) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return 0;
}

static int
parse_row(const char *line, soak_row_t *r)
{
    uint64_t u;

    memset(r, 0, sizeof(*r));
    (void)j_str(line, "id", r->id, sizeof(r->id));
    (void)j_str(line, "tag", r->tag, sizeof(r->tag));
    (void)j_str(line, "sig", r->sighex, sizeof(r->sighex));
    (void)j_u64(line, "slot", &r->slot);
    (void)j_u64(line, "ain", &r->ain);
    (void)j_u64(line, "rb", &r->rb);
    (void)j_u64(line, "rq", &r->rq);
    (void)j_u64(line, "prb", &r->prb);
    (void)j_u64(line, "prq", &r->prq);
    (void)j_i64(line, "vq", &r->vq);
    (void)j_u64(line, "lp", &r->lp);
    (void)j_u64(line, "proto", &r->proto);
    (void)j_u64(line, "cr", &r->cr);
    if (j_u64(line, "n_cpi", &u) == 0) {
        r->n_cpi = (uint32_t)u;
    } else {
        r->n_cpi = 1;
    }
    if (j_u64(line, "dir", &u) == 0) {
        r->dir = (uint8_t)u;
    }
    if (j_u64(line, "ix", &u) == 0) {
        r->ix = (uint8_t)u;
    }
    if (j_u64(line, "pre_complete", &u) == 0) {
        r->pre_complete = (uint8_t)u;
    }
    if (j_u64(line, "wsol", &u) == 0) {
        r->wsol = (uint8_t)u;
    }
    if (j_u64(line, "t22", &u) == 0) {
        r->t22 = (uint8_t)u;
    }
    if (r->id[0] == '\0' || r->rb == 0 || r->prb == 0) {
        return -1;
    }
    if (r->ix == 0) {
        r->ix = (r->dir == PUMP_DIR_BASE_TO_QUOTE) ? SOAK_IX_SELL
                                                   : SOAK_IX_BUY_EQ;
    }
    return 0;
}

static int
hex_sig(const char *hex, uint8_t sig[STATE_SIG_LEN])
{
    size_t n, i;

    memset(sig, 0, STATE_SIG_LEN);
    if (hex == NULL) {
        return 0;
    }
    n = strlen(hex);
    if (n > 128) {
        n = 128;
    }
    for (i = 0; i + 1 < n && i / 2 < STATE_SIG_LEN; i += 2) {
        unsigned int b;
        if (sscanf(hex + i, "%2x", &b) != 1) {
            return -1;
        }
        sig[i / 2] = (uint8_t)b;
    }
    return 0;
}

static int
fill_pool(compact_state_t *st, uint64_t rb, uint64_t rq, int64_t vq,
          uint64_t lp, uint64_t proto, uint64_t cr, uint64_t inc)
{
    pump_state_t s;
    uint32_t id = 0;

    memset(&s, 0, sizeof(s));
    s.reserve_base = rb;
    s.reserve_quote = rq;
    s.virtual_quote = vq;
    s.lp_fee_bps = lp;
    s.protocol_fee_bps = proto;
    s.creator_fee_bps = cr;
    if (compact_pool_add_pump(st, NULL, &s, &id) != 0) {
        return -1;
    }
    st->pool[id].incarnation = inc;
    return 0;
}

static int
row_to_case(const soak_row_t *r, oracle_case_t *c)
{
    ordered_ix_t *ix;

    memset(c, 0, sizeof(*c));
    snprintf(c->id, sizeof(c->id), "%s", r->id);
    c->pre_complete = r->pre_complete;
    c->source = 0;
    c->pre = calloc(1, sizeof(*c->pre));
    c->post = calloc(1, sizeof(*c->post));
    if (c->pre == NULL || c->post == NULL) {
        return -1;
    }
    ordered_tx_clear(&c->tx);
    c->tx.slot = r->slot;
    (void)hex_sig(r->sighex, c->tx.sig);
    if (fill_pool(c->pre, r->rb, r->rq, r->vq, r->lp, r->proto, r->cr, 1) != 0
        || fill_pool(c->post, r->prb, r->prq, r->vq, r->lp, r->proto, r->cr,
                     r->pre_complete ? 2 : 1)
            != 0) {
        return -1;
    }
    ix = &c->tx.ix[0];
    memset(ix, 0, sizeof(*ix));
    ix->relevant = 1;
    ix->direction = r->dir;
    ix->amount_in = r->ain;
    ix->pool_id = 0;
    ix->src_token = STATE_ACCT_NONE;
    ix->dst_token = STATE_ACCT_NONE;
    ix->vault_base = STATE_ACCT_NONE;
    ix->vault_quote = STATE_ACCT_NONE;
    ix->fee_proto = STATE_ACCT_NONE;
    ix->fee_creator = STATE_ACCT_NONE;
    ix->src_sys = STATE_ACCT_NONE;
    ix->dst_sys = STATE_ACCT_NONE;
    if (r->ix == SOAK_IX_SELL) {
        ix->proto = PROTO_PUMP;
        ix->kind = IX_KIND_PUMP_SELL;
        ix->direction = PUMP_DIR_BASE_TO_QUOTE;
    } else if (r->ix == SOAK_IX_BUY_EQ) {
        ix->proto = PROTO_PUMP;
        ix->kind = IX_KIND_PUMP_BUY_EQ;
        ix->direction = PUMP_DIR_QUOTE_TO_BASE;
    } else if (r->ix == SOAK_IX_BUY_OUT) {
        ix->proto = PROTO_PUMP;
        ix->kind = IX_KIND_PUMP_BUY;
        ix->direction = PUMP_DIR_QUOTE_TO_BASE;
    } else if (r->ix == SOAK_IX_DLMM) {
        ix->proto = PROTO_DLMM;
        ix->kind = IX_KIND_DLMM_SWAP;
    } else {
        ix->proto = PROTO_PUMP;
        ix->kind = IX_KIND_OTHER;
    }
    c->tx.n_ix = 1;
    return 0;
}

int
oracle_admit_jsonl(const char *path, oracle_soak_stats_t *st)
{
    FILE *f;
    char line[4096];
    static const char *const fb[] = {
        "tests/fixtures/oracle/pump-oracle-002.jsonl",
        "../tests/fixtures/oracle/pump-oracle-002.jsonl",
    };
    size_t i;

    if (st == NULL) {
        return -1;
    }
    oracle_soak_stats_clear(st);
    f = NULL;
    if (path != NULL) {
        f = fopen(path, "r");
    }
    for (i = 0; f == NULL && i < 2; i++) {
        f = fopen(fb[i], "r");
    }
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        soak_row_t row;
        oracle_case_t c;
        oracle_case_result_t ar;
        int64_t vrec;

        if (line[0] != '{') {
            continue;
        }
        if (parse_row(line, &row) != 0) {
            st->skipped++;
            continue;
        }
        if (row_to_case(&row, &c) != 0) {
            oracle_case_clear(&c);
            st->skipped++;
            continue;
        }
        if (oracle_run_case(&c, &ar) != 0) {
            oracle_case_clear(&c);
            st->unexplained++;
            st->usable++;
            continue;
        }
        st->usable++;
        if (ar.judged == ORACLE_FAST_CUSTOM_EXACT) {
            st->exact++;
            if (ar.diff.n_pool_mismatch || ar.diff.n_token_mismatch
                || ar.diff.n_sys_mismatch || ar.diff.n_incarnation_mismatch
                || ar.diff.n_cert_mismatch || ar.diff.n_overlay_mismatch) {
                st->exact_wrong++;
                st->exact--;
                st->unexplained++;
            }
        } else if (ar.judged == ORACLE_FALLBACK_REQUIRED) {
            st->fallback++;
        } else if (ar.judged == ORACLE_UNKNOWN) {
            st->unknown++;
        } else {
            st->unexplained++;
        }
        if (row.ix == SOAK_IX_SELL) {
            st->sell++;
        } else if (row.ix == SOAK_IX_BUY_EQ) {
            st->buy_eq++;
        } else if (row.ix == SOAK_IX_BUY_OUT) {
            st->buy_out++;
        }
        if (row.n_cpi > 1) {
            st->multi_cpi++;
        }
        if (row.wsol) {
            st->wsol++;
        }
        if (strcmp(row.tag, "assoc") == 0) {
            st->assoc++;
        }
        if (strcmp(row.tag, "missing_virtual") == 0) {
            st->missing_v++;
        }
        if (strcmp(row.tag, "missing_fee") == 0) {
            st->missing_fee++;
        }
        if (strcmp(row.tag, "wrong_n") == 0) {
            st->wrong_n++;
        }
        if (row.t22) {
            st->token2022++;
        }
        if (row.ix == SOAK_IX_OTHER || strcmp(row.tag, "unknown_disc") == 0) {
            st->unsupported++;
        }
        if (row.ix == SOAK_IX_SELL || row.ix == SOAK_IX_BUY_EQ) {
            pump_state_t before;

            memset(&before, 0, sizeof(before));
            before.reserve_base = row.rb;
            before.reserve_quote = row.rq;
            before.virtual_quote = 0;
            before.lp_fee_bps = row.lp;
            before.protocol_fee_bps = row.proto;
            before.creator_fee_bps = row.cr;
            if (pump_invert_virtual(&before, row.ain, row.dir, row.prb,
                                    row.prq, &vrec) == 0) {
                st->recovered_v++;
            }
        }
        oracle_case_clear(&c);
    }
    fclose(f);
    return 0;
}

void
oracle_soak_print(const oracle_soak_stats_t *st)
{
    double n;

    if (st == NULL) {
        return;
    }
    n = (st->usable != 0) ? (double)st->usable : 1.0;
    printf("PUMP-ORACLE-002\n");
    printf("  usable              %u\n", st->usable);
    printf("  FAST_CUSTOM_EXACT   %u  (%.2f%%)\n", st->exact,
           100.0 * (double)st->exact / n);
    printf("  FALLBACK_REQUIRED   %u  (%.2f%%)\n", st->fallback,
           100.0 * (double)st->fallback / n);
    printf("  UNKNOWN             %u  (%.2f%%)\n", st->unknown,
           100.0 * (double)st->unknown / n);
    printf("  UNEXPLAINED         %u  (%.2f%%)\n", st->unexplained,
           100.0 * (double)st->unexplained / n);
    printf("  FAST_CUSTOM_EXACT but wrong  %u\n", st->exact_wrong);
    printf("  sell                %u\n", st->sell);
    printf("  buy_exact_quote_in  %u\n", st->buy_eq);
    printf("  buy_exact_out       %u\n", st->buy_out);
    printf("  multi-CPI           %u\n", st->multi_cpi);
    printf("  WSOL/system         %u\n", st->wsol);
    printf("  association         %u\n", st->assoc);
    printf("  virtual miss        %u\n", st->missing_v);
    printf("  fee/config miss     %u\n", st->missing_fee);
    printf("  invert(V) hits      %u\n", st->recovered_v);
    printf("  wrong_n tag         %u\n", st->wrong_n);
    printf("  Token-2022          %u\n", st->token2022);
    printf("  unsupported branch  %u\n", st->unsupported);
    printf("  skipped             %u\n", st->skipped);
}
