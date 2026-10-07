#include "oracle/case.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
oracle_case_clear(oracle_case_t *c)
{
    if (c == NULL) {
        return;
    }
    free(c->pre);
    free(c->post);
    memset(c, 0, sizeof(*c));
}

static int
fields_exact(const oracle_diff_t *d)
{
    return d->n_pool_mismatch == 0 && d->n_token_mismatch == 0
        && d->n_sys_mismatch == 0 && d->n_incarnation_mismatch == 0
        && d->n_cert_mismatch == 0 && d->n_overlay_mismatch == 0;
}

int
oracle_run_case(const oracle_case_t *c, oracle_case_result_t *out)
{
    compact_state_t *work;
    apply_result_t ar;
    int rc, exact;

    if (c == NULL || out == NULL || c->pre == NULL || c->post == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->id, c->id, ORACLE_CASE_ID_MAX);
    out->expect = c->expect;
    work = malloc(sizeof(*work));
    if (work == NULL) {
        return -1;
    }
    *work = *c->pre;
    rc = apply_tx(work, &c->tx, &ar);
    out->apply_rc = rc;
    out->dep_class = ar.dep_class;
    exact = 0;
    if (rc == APPLY_OK || rc == APPLY_IRRELEVANT) {
        (void)oracle_diff_state(work, c->post, &out->diff);
        (void)oracle_diff_overlay(&ar.overlay, c->post, &out->diff);
        if (rc == APPLY_OK) {
            (void)oracle_diff_cert(&ar.cert, &c->tx, DEP_FAST_CUSTOM,
                                   &out->diff);
        }
        exact = fields_exact(&out->diff);
    }
    out->diff.class_ours = ar.dep_class;
    out->diff.class_expect = c->expect;
    out->judged = oracle_judge(rc, ar.dep_class, c->pre_complete, exact);
    out->diff.judged = out->judged;
    free(work);
    return 0;
}

static uint64_t
uval(const char *s, const char *key, uint64_t def)
{
    const char *p;
    char pat[40];
    unsigned long long v;

    if (s == NULL || key == NULL) {
        return def;
    }
    snprintf(pat, sizeof(pat), "%s=", key);
    p = strstr(s, pat);
    if (p == NULL) {
        return def;
    }
    p += strlen(pat);
    if (strcmp(p, "NONE") == 0 || strncmp(p, "NONE", 4) == 0) {
        return STATE_ACCT_NONE;
    }
    if (sscanf(p, "%llu", &v) != 1) {
        return def;
    }
    return (uint64_t)v;
}

static uint8_t
parse_expect(const char *s)
{
    if (strstr(s, "FAST_CUSTOM_EXACT") != NULL) {
        return ORACLE_FAST_CUSTOM_EXACT;
    }
    if (strstr(s, "FALLBACK_REQUIRED") != NULL) {
        return ORACLE_FALLBACK_REQUIRED;
    }
    if (strstr(s, "UNKNOWN") != NULL) {
        return ORACLE_UNKNOWN;
    }
    return ORACLE_UNEXPLAINED;
}

static uint8_t
parse_kind(const char *s)
{
    if (strstr(s, "kind=sell") != NULL) {
        return IX_KIND_PUMP_SELL;
    }
    if (strstr(s, "kind=buy_eq") != NULL) {
        return IX_KIND_PUMP_BUY_EQ;
    }
    if (strstr(s, "kind=buy_out") != NULL) {
        return IX_KIND_PUMP_BUY;
    }
    if (strstr(s, "kind=xfer") != NULL) {
        return IX_KIND_TOKEN_XFER;
    }
    if (strstr(s, "kind=sync") != NULL) {
        return IX_KIND_TOKEN_SYNC;
    }
    if (strstr(s, "kind=close") != NULL) {
        return IX_KIND_TOKEN_CLOSE;
    }
    if (strstr(s, "kind=ata") != NULL) {
        return IX_KIND_ATA_CREATE;
    }
    if (strstr(s, "kind=sys") != NULL) {
        return IX_KIND_SYS_TRANSFER;
    }
    if (strstr(s, "kind=dlmm") != NULL) {
        return IX_KIND_DLMM_SWAP;
    }
    return IX_KIND_OTHER;
}

static uint8_t
parse_proto(uint8_t kind)
{
    if (kind == IX_KIND_PUMP_SELL || kind == IX_KIND_PUMP_BUY_EQ
        || kind == IX_KIND_PUMP_BUY || kind == IX_KIND_OTHER) {
        return PROTO_PUMP;
    }
    if (kind == IX_KIND_DLMM_SWAP) {
        return PROTO_DLMM;
    }
    if (kind == IX_KIND_SYS_TRANSFER) {
        return PROTO_SYSTEM;
    }
    if (kind == IX_KIND_ATA_CREATE) {
        return PROTO_ATA;
    }
    return PROTO_TOKEN;
}

static void
ix_none(ordered_ix_t *ix)
{
    ix->pool_id = STATE_ACCT_NONE;
    ix->src_token = STATE_ACCT_NONE;
    ix->dst_token = STATE_ACCT_NONE;
    ix->vault_base = STATE_ACCT_NONE;
    ix->vault_quote = STATE_ACCT_NONE;
    ix->fee_proto = STATE_ACCT_NONE;
    ix->fee_creator = STATE_ACCT_NONE;
    ix->src_sys = STATE_ACCT_NONE;
    ix->dst_sys = STATE_ACCT_NONE;
}

static int
hex_sig(const char *hex, uint8_t sig[STATE_SIG_LEN])
{
    size_t n = strlen(hex);
    size_t i;

    memset(sig, 0, STATE_SIG_LEN);
    if (n > 128) {
        n = 128;
    }
    if (n % 2 != 0) {
        return -1;
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
ensure_states(oracle_case_t *c)
{
    if (c->pre == NULL) {
        c->pre = calloc(1, sizeof(*c->pre));
    }
    if (c->post == NULL) {
        c->post = calloc(1, sizeof(*c->post));
    }
    return (c->pre != NULL && c->post != NULL) ? 0 : -1;
}

static int
add_pool(compact_state_t *st, const char *line)
{
    pump_state_t s;
    uint32_t id;

    memset(&s, 0, sizeof(s));
    s.reserve_base = uval(line, "rb", 0);
    s.reserve_quote = uval(line, "rq", 0);
    s.virtual_quote = (int64_t)uval(line, "vq", 0);
    s.lp_fee_bps = uval(line, "lp", 0);
    s.protocol_fee_bps = uval(line, "proto", 0);
    s.creator_fee_bps = uval(line, "cr", 0);
    if (compact_pool_add_pump(st, NULL, &s, &id) != 0) {
        return -1;
    }
    st->pool[id].incarnation = uval(line, "inc", 1);
    return 0;
}

static int
add_token(compact_state_t *st, const char *line)
{
    state_token_t t;
    uint32_t id;

    memset(&t, 0, sizeof(t));
    t.amount = uval(line, "amt", 0);
    t.native_reserve = uval(line, "native", 0);
    t.flags = (uint8_t)uval(line, "wsol", 0) ? TOKEN_FLAG_WSOL : 0;
    t.live = (uint8_t)uval(line, "live", 1);
    t.token_program = TOKEN_PROG_SPL;
    t.extensions_mask = (uint16_t)uval(line, "ext", 0);
    if (t.extensions_mask != 0) {
        t.token_program = TOKEN_PROG_2022;
    }
    t.mint[0] = (uint8_t)uval(line, "mint", 1);
    if (compact_token_add(st, &t, &id) != 0) {
        return -1;
    }
    st->token[id].incarnation = uval(line, "inc", 1);
    st->token[id].live = t.live;
    return 0;
}

static int
add_sys(compact_state_t *st, const char *line)
{
    state_sys_t s;
    uint32_t id;

    memset(&s, 0, sizeof(s));
    s.lamports = uval(line, "lam", 0);
    if (compact_sys_add(st, &s, &id) != 0) {
        return -1;
    }
    st->sys[id].incarnation = uval(line, "inc", 1);
    return 0;
}

static FILE *
open_corpus(const char *path)
{
    static const char *const fb[] = {
        "tests/fixtures/oracle/pump-oracle-001.txt",
        "../tests/fixtures/oracle/pump-oracle-001.txt",
    };
    FILE *f = NULL;
    size_t i;

    if (path != NULL) {
        f = fopen(path, "r");
    }
    for (i = 0; f == NULL && i < 2; i++) {
        f = fopen(fb[i], "r");
    }
    return f;
}

int
oracle_load_corpus(const char *path, oracle_case_t *out, uint32_t cap)
{
    FILE *f;
    char line[1024];
    oracle_case_t *cur = NULL;
    uint32_t n = 0;

    if (out == NULL || cap == 0) {
        return -1;
    }
    f = open_corpus(path);
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (strncmp(line, "CASE ", 5) == 0) {
            if (n >= cap) {
                break;
            }
            cur = &out[n];
            memset(cur, 0, sizeof(*cur));
            ordered_tx_clear(&cur->tx);
            {
                const char *idp = strstr(line, "id=");
                if (idp != NULL) {
                    sscanf(idp + 3, "%63s", cur->id);
                }
            }
            if (strstr(line, "source=geyser") != NULL) {
                cur->source = 0;
            } else if (strstr(line, "source=sdk") != NULL) {
                cur->source = 1;
            } else if (strstr(line, "source=overlay016") != NULL) {
                cur->source = 2;
            } else {
                cur->source = 3;
            }
            cur->pre_complete = (uint8_t)uval(line, "pre_complete", 1);
            cur->expect = parse_expect(line);
            if (ensure_states(cur) != 0) {
                fclose(f);
                return -1;
            }
            n++;
            continue;
        }
        if (cur == NULL) {
            continue;
        }
        if (strncmp(line, "END", 3) == 0) {
            cur = NULL;
            continue;
        }
        if (strncmp(line, "SLOT ", 5) == 0) {
            cur->tx.slot = uval(line + 5, "v", strtoull(line + 5, NULL, 10));
            if (strstr(line, "v=") == NULL) {
                cur->tx.slot = strtoull(line + 5, NULL, 10);
            }
            continue;
        }
        if (strncmp(line, "TXINDEX ", 8) == 0) {
            cur->tx.tx_index = (uint32_t)strtoul(line + 8, NULL, 10);
            continue;
        }
        if (strncmp(line, "SIGHEX ", 7) == 0) {
            char hex[129];
            memset(hex, 0, sizeof(hex));
            sscanf(line + 7, "%128s", hex);
            (void)hex_sig(hex, cur->tx.sig);
            continue;
        }
        if (strncmp(line, "POOLPRE ", 8) == 0) {
            (void)add_pool(cur->pre, line);
            continue;
        }
        if (strncmp(line, "POOLPOST ", 9) == 0) {
            (void)add_pool(cur->post, line);
            continue;
        }
        if (strncmp(line, "TOKENPRE ", 9) == 0) {
            (void)add_token(cur->pre, line);
            continue;
        }
        if (strncmp(line, "TOKENPOST ", 10) == 0) {
            (void)add_token(cur->post, line);
            continue;
        }
        if (strncmp(line, "SYSPRE ", 7) == 0) {
            (void)add_sys(cur->pre, line);
            continue;
        }
        if (strncmp(line, "SYSPOST ", 8) == 0) {
            (void)add_sys(cur->post, line);
            continue;
        }
        if (strncmp(line, "IX ", 3) == 0) {
            ordered_ix_t *ix;
            if (cur->tx.n_ix >= STATE_IX_MAX) {
                continue;
            }
            ix = &cur->tx.ix[cur->tx.n_ix];
            memset(ix, 0, sizeof(*ix));
            ix_none(ix);
            ix->kind = parse_kind(line);
            ix->proto = parse_proto(ix->kind);
            if (strstr(line, "proto=token") != NULL) {
                ix->proto = PROTO_TOKEN;
            }
            ix->relevant = (uint8_t)uval(line, "rel", 1);
            ix->direction = (uint8_t)uval(line, "dir", 1);
            ix->amount_in = uval(line, "ain", 0);
            ix->min_out = uval(line, "min", 0);
            ix->pool_id = (uint32_t)uval(line, "pool", STATE_ACCT_NONE);
            ix->src_token = (uint32_t)uval(line, "src", STATE_ACCT_NONE);
            ix->dst_token = (uint32_t)uval(line, "dst", STATE_ACCT_NONE);
            ix->vault_base = (uint32_t)uval(line, "vb", STATE_ACCT_NONE);
            ix->vault_quote = (uint32_t)uval(line, "vq", STATE_ACCT_NONE);
            ix->src_sys = (uint32_t)uval(line, "ss", STATE_ACCT_NONE);
            ix->dst_sys = (uint32_t)uval(line, "ds", STATE_ACCT_NONE);
            cur->tx.n_ix++;
        }
    }
    fclose(f);
    return (int)n;
}
