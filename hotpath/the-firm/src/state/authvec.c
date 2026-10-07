#include "state/authvec.h"

#include "shred/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int
unhex(const char *s, uint8_t *out, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        int a, b;

        a = hexval(s[2u * i]);
        b = hexval(s[2u * i + 1u]);
        if (a < 0 || b < 0) {
            return -1;
        }
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static int
pk_zero(const uint8_t pk[32])
{
    static const uint8_t z[32];

    return memcmp(pk, z, 32) == 0;
}

static const char *
json_qstr(const char *line, const char *key)
{
    const char *p = strstr(line, key);

    if (p == NULL) {
        return NULL;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return NULL;
    }
    return p + 1;
}

static int
json_u64(const char *line, const char *key, uint64_t *out)
{
    const char *p = strstr(line, key);
    char *end;
    unsigned long long v;

    if (p == NULL || out == NULL) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    v = strtoull(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int
json_i64(const char *line, const char *key, int64_t *out)
{
    const char *p = strstr(line, key);
    char *end;
    long long v;

    if (p == NULL || out == NULL) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    v = strtoll(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

void
authvec_clear(authvec_t *av)
{
    if (av == NULL) {
        return;
    }
    memset(av, 0, sizeof(*av));
}

int
authvec_note_pred(authvec_t *av, const compact_state_t *st,
                  const ordered_tx_t *tx, uint32_t pool_id,
                  const pump_state_t *predicted)
{
    return authvec_note_pred_full(av, st, tx, pool_id, NULL, predicted, 0, 0);
}

int
authvec_note_pred_full(authvec_t *av, const compact_state_t *st,
                       const ordered_tx_t *tx, uint32_t pool_id,
                       const pump_state_t *prestate,
                       const pump_state_t *predicted, uint64_t of_rx_tsc,
                       uint64_t state_ready_tsc)
{
    auth_pred_t *p;
    const state_pool_t *row;

    if (av == NULL || tx == NULL || predicted == NULL
        || av->n_pred >= AUTH_PRED_MAX) {
        return -1;
    }
    if (compact_pool_get(st, pool_id, &row) != 0) {
        return -1;
    }
    p = &av->pred[av->n_pred];
    memset(p, 0, sizeof(*p));
    memcpy(p->sig, tx->sig, STATE_SIG_LEN);
    p->slot = tx->slot;
    p->pool_id = pool_id;
    memcpy(p->pool, row->pubkey, 32);
    memcpy(p->vault_base, row->vault_base, 32);
    memcpy(p->vault_quote, row->vault_quote, 32);
    p->need_vb = (uint8_t)!pk_zero(row->vault_base);
    p->need_vq = (uint8_t)!pk_zero(row->vault_quote);
    p->predicted = *predicted;
    p->incarnation = row->incarnation;
    p->of_rx_tsc = of_rx_tsc;
    p->state_ready_tsc = state_ready_tsc;
    if (prestate != NULL) {
        p->prestate = *prestate;
        shred_sha256((const uint8_t *)prestate, sizeof(*prestate),
                     p->prestate_hash);
    }
    p->judged = AUTH_PEND;
    av->n_pred++;
    return 0;
}

int
authvec_put_write(authvec_t *av, const auth_write_t *w)
{
    if (av == NULL || w == NULL || av->n_write >= AUTH_WRITE_MAX) {
        return -1;
    }
    av->write[av->n_write] = *w;
    av->n_write++;
    return 0;
}

static int
parse_write_line(const char *line, auth_write_t *w)
{
    const char *hs, *ps, *rs;
    uint64_t u;

    memset(w, 0, sizeof(*w));
    hs = json_qstr(line, "\"sig_hex\"");
    ps = json_qstr(line, "\"pubkey_hex\"");
    if (hs == NULL || ps == NULL || unhex(hs, w->sig, 64) != 0
        || unhex(ps, w->pubkey, 32) != 0) {
        return -1;
    }
    (void)json_u64(line, "\"slot\"", &w->slot);
    (void)json_u64(line, "\"write_version\"", &w->write_version);
    rs = json_qstr(line, "\"role\"");
    if (rs != NULL) {
        if (strncmp(rs, "pool", 4) == 0) {
            w->role = AUTH_ROLE_POOL;
        } else if (strncmp(rs, "vault_base", 10) == 0) {
            w->role = AUTH_ROLE_VB;
        } else if (strncmp(rs, "vault_quote", 11) == 0) {
            w->role = AUTH_ROLE_VQ;
        }
    }
    if (json_u64(line, "\"amount\"", &w->amount) == 0) {
        w->have_amount = 1;
    }
    if (json_u64(line, "\"reserve_base\"", &w->pump.reserve_base) == 0
        && json_u64(line, "\"reserve_quote\"", &w->pump.reserve_quote) == 0) {
        w->have_reserves = 1;
    }
    if (json_i64(line, "\"virtual_quote\"", &w->pump.virtual_quote) == 0) {
        w->have_vq = 1;
    }
    if (json_u64(line, "\"lp_fee_bps\"", &w->pump.lp_fee_bps) == 0) {
        w->have_fees = 1;
        (void)json_u64(line, "\"protocol_fee_bps\"", &w->pump.protocol_fee_bps);
        (void)json_u64(line, "\"creator_fee_bps\"", &w->pump.creator_fee_bps);
    }
    if (json_u64(line, "\"disabled\"", &u) == 0) {
        w->pump.disabled = (uint8_t)u;
        w->have_disabled = 1;
    }
    if (json_u64(line, "\"status\"", &u) == 0) {
        w->pump.status = (uint8_t)u;
        w->have_status = 1;
    }
    return 0;
}

int
authvec_load_jsonl_from(authvec_t *av, const char *path, uint64_t *off)
{
    FILE *f;
    char line[4096];

    if (av == NULL || path == NULL) {
        return -1;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    if (off != NULL && *off != 0 && fseek(f, (long)*off, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        auth_write_t w;

        if (parse_write_line(line, &w) != 0) {
            continue;
        }
        if (authvec_put_write(av, &w) != 0) {
            break;
        }
    }
    if (off != NULL) {
        long pos = ftell(f);

        if (pos >= 0) {
            *off = (uint64_t)pos;
        }
    }
    fclose(f);
    return 0;
}

int
authvec_load_jsonl(authvec_t *av, const char *path)
{
    return authvec_load_jsonl_from(av, path, NULL);
}

static int
same32(const uint8_t a[32], const uint8_t b[32])
{
    return memcmp(a, b, 32) == 0;
}

static int
collect(const authvec_t *av, const auth_pred_t *p, const uint8_t pk[32],
        auth_write_t *out)
{
    uint32_t i, n = 0;
    const auth_write_t *hit = NULL;

    for (i = 0; i < av->n_write; i++) {
        const auth_write_t *w = &av->write[i];

        if (memcmp(w->sig, p->sig, STATE_SIG_LEN) != 0) {
            continue;
        }
        if (!same32(w->pubkey, pk)) {
            continue;
        }
        if (w->slot != p->slot) {
            return -2;
        }
        if (hit != NULL && (hit->write_version != w->write_version
                            || memcmp(&hit->pump, &w->pump,
                                      sizeof(w->pump)) != 0
                            || hit->amount != w->amount)) {
            return -2;
        }
        hit = w;
        n++;
    }
    if (n == 0 || hit == NULL) {
        return -1;
    }
    *out = *hit;
    return 0;
}

static int
vec_exact(const auth_pred_t *p, const auth_write_t *pool,
          const auth_write_t *vb, const auth_write_t *vq, uint8_t *mixed)
{
    uint64_t base, quote;

    *mixed = 0;
    if (pool == NULL || !pool->have_vq) {
        return AUTH_INCOMPLETE;
    }
    if (p->need_vb) {
        if (vb == NULL || !vb->have_amount) {
            return AUTH_INCOMPLETE;
        }
        base = vb->amount;
    } else if (pool->have_reserves) {
        base = pool->pump.reserve_base;
    } else {
        return AUTH_INCOMPLETE;
    }
    if (p->need_vq) {
        if (vq == NULL || !vq->have_amount) {
            return AUTH_INCOMPLETE;
        }
        quote = vq->amount;
    } else if (pool->have_reserves) {
        quote = pool->pump.reserve_quote;
    } else {
        return AUTH_INCOMPLETE;
    }
    if (pool->have_reserves
        && (pool->pump.reserve_base != base
            || pool->pump.reserve_quote != quote)) {
        *mixed = 1;
        return AUTH_INCOMPLETE;
    }
    if (p->predicted.reserve_base != base
        || p->predicted.reserve_quote != quote
        || p->predicted.virtual_quote != pool->pump.virtual_quote) {
        return AUTH_MISMATCH;
    }
    if (pool->have_fees
        && (p->predicted.lp_fee_bps != pool->pump.lp_fee_bps
            || p->predicted.protocol_fee_bps != pool->pump.protocol_fee_bps
            || p->predicted.creator_fee_bps != pool->pump.creator_fee_bps)) {
        return AUTH_MISMATCH;
    }
    if (pool->have_disabled && p->predicted.disabled != pool->pump.disabled) {
        return AUTH_MISMATCH;
    }
    if (pool->have_status && p->predicted.status != pool->pump.status) {
        return AUTH_MISMATCH;
    }
    return AUTH_EXACT;
}

void
authvec_judge(authvec_t *av)
{
    uint32_t i;

    if (av == NULL) {
        return;
    }
    av->n_exact = 0;
    av->n_mismatch = 0;
    av->n_incomplete = 0;
    av->n_mixed = 0;
    for (i = 0; i < av->n_pred; i++) {
        auth_pred_t *p = &av->pred[i];
        auth_write_t pool, vb, vq;
        auth_write_t *pp = NULL, *vbp = NULL, *vqp = NULL;
        uint8_t mixed = 0;
        int rc;

        rc = collect(av, p, p->pool, &pool);
        if (rc == 0) {
            pp = &pool;
        } else if (rc == -2) {
            p->judged = AUTH_INCOMPLETE;
            av->n_incomplete++;
            av->n_mixed++;
            continue;
        }
        if (p->need_vb) {
            rc = collect(av, p, p->vault_base, &vb);
            if (rc == 0) {
                vbp = &vb;
            } else if (rc == -2) {
                p->judged = AUTH_INCOMPLETE;
                av->n_incomplete++;
                av->n_mixed++;
                continue;
            }
        }
        if (p->need_vq) {
            rc = collect(av, p, p->vault_quote, &vq);
            if (rc == 0) {
                vqp = &vq;
            } else if (rc == -2) {
                p->judged = AUTH_INCOMPLETE;
                av->n_incomplete++;
                av->n_mixed++;
                continue;
            }
        }
        p->judged = (uint8_t)vec_exact(p, pp, vbp, vqp, &mixed);
        if (mixed) {
            av->n_mixed++;
        }
        if (p->judged == AUTH_EXACT) {
            av->n_exact++;
        } else if (p->judged == AUTH_MISMATCH) {
            av->n_mismatch++;
        } else {
            av->n_incomplete++;
        }
    }
}
