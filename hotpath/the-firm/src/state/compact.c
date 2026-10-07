#include "state/compact.h"

#include <string.h>

void
compact_state_clear(compact_state_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

int
compact_pool_add_pump(compact_state_t *st, const uint8_t pubkey[32],
                      const pump_state_t *s, uint32_t *id_out)
{
    state_pool_t *p;

    if (st == NULL || s == NULL || st->n_pool >= STATE_POOL_MAX) {
        return -1;
    }
    p = &st->pool[st->n_pool];
    memset(p, 0, sizeof(*p));
    if (pubkey != NULL) {
        memcpy(p->pubkey, pubkey, 32);
    }
    p->proto = STATE_PROTO_PUMP;
    p->live = 1;
    p->grade = POOL_GRADE_EXACT;
    p->incarnation = 1;
    p->anchor_slot = 0;
    p->pump = *s;
    if (id_out != NULL) {
        *id_out = st->n_pool;
    }
    st->n_pool++;
    return 0;
}

static int
upsert_vault(compact_state_t *st, const uint8_t pk[32], uint64_t amount)
{
    uint32_t tid;
    state_token_t tok;

    if (compact_token_find(st, pk, &tid) == 0) {
        st->token[tid].amount = amount;
        st->token[tid].live = 1;
        return 0;
    }
    memset(&tok, 0, sizeof(tok));
    memcpy(tok.pubkey, pk, 32);
    tok.amount = amount;
    tok.token_program = TOKEN_PROG_SPL;
    tok.live = 1;
    tok.incarnation = 1;
    return compact_token_add(st, &tok, NULL);
}

int
compact_pool_arm(compact_state_t *st, const uint8_t pool[32],
                 const uint8_t vault_base[32], const uint8_t vault_quote[32],
                 const pump_state_t *s, uint64_t anchor_slot, uint32_t *id_out)
{
    uint32_t id;

    if (st == NULL || pool == NULL || s == NULL) {
        return -1;
    }
    if (compact_pool_find(st, pool, &id) != 0) {
        if (compact_pool_add_pump(st, pool, s, &id) != 0) {
            return -1;
        }
    } else {
        st->pool[id].incarnation += 1;
        if (st->pool[id].incarnation == 0) {
            st->pool[id].incarnation = 1;
        }
    }
    st->pool[id].auth_bits = POOL_AUTH_READY;
    st->pool[id].grade = POOL_GRADE_EXACT;
    st->pool[id].live = 1;
    st->pool[id].anchor_slot = anchor_slot;
    st->pool[id].pump = *s;
    if (vault_base != NULL) {
        memcpy(st->pool[id].vault_base, vault_base, 32);
        if (upsert_vault(st, vault_base, s->reserve_base) != 0) {
            return -1;
        }
    }
    if (vault_quote != NULL) {
        memcpy(st->pool[id].vault_quote, vault_quote, 32);
        if (upsert_vault(st, vault_quote, s->reserve_quote) != 0) {
            return -1;
        }
    }
    if (id_out != NULL) {
        *id_out = id;
    }
    return 0;
}

int
compact_pool_get(const compact_state_t *st, uint32_t id,
                 const state_pool_t **out)
{
    if (st == NULL || out == NULL || id >= st->n_pool) {
        return -1;
    }
    *out = &st->pool[id];
    return 0;
}

int
compact_token_add(compact_state_t *st, const state_token_t *t, uint32_t *id_out)
{
    state_token_t *d;

    if (st == NULL || t == NULL || st->n_token >= STATE_TOKEN_MAX) {
        return -1;
    }
    d = &st->token[st->n_token];
    *d = *t;
    if (d->incarnation == 0) {
        d->incarnation = 1;
    }
    d->live = 1;
    if (id_out != NULL) {
        *id_out = st->n_token;
    }
    st->n_token++;
    return 0;
}

int
compact_token_reserve(compact_state_t *st, const uint8_t pubkey[32],
                      uint32_t *id_out)
{
    state_token_t *d;

    if (st == NULL || st->n_token >= STATE_TOKEN_MAX) {
        return -1;
    }
    d = &st->token[st->n_token];
    memset(d, 0, sizeof(*d));
    if (pubkey != NULL) {
        memcpy(d->pubkey, pubkey, 32);
    }
    d->live = 0;
    d->incarnation = 1;
    if (id_out != NULL) {
        *id_out = st->n_token;
    }
    st->n_token++;
    return 0;
}

int
compact_token_get(const compact_state_t *st, uint32_t id,
                  const state_token_t **out)
{
    if (st == NULL || out == NULL || id >= st->n_token) {
        return -1;
    }
    *out = &st->token[id];
    return 0;
}

int
compact_sys_add(compact_state_t *st, const state_sys_t *s, uint32_t *id_out)
{
    state_sys_t *d;

    if (st == NULL || s == NULL || st->n_sys >= STATE_SYS_MAX) {
        return -1;
    }
    d = &st->sys[st->n_sys];
    *d = *s;
    if (d->incarnation == 0) {
        d->incarnation = 1;
    }
    d->live = 1;
    if (id_out != NULL) {
        *id_out = st->n_sys;
    }
    st->n_sys++;
    return 0;
}

int
compact_sys_get(const compact_state_t *st, uint32_t id, const state_sys_t **out)
{
    if (st == NULL || out == NULL || id >= st->n_sys) {
        return -1;
    }
    *out = &st->sys[id];
    return 0;
}

int
compact_pool_find(const compact_state_t *st, const uint8_t pk[32],
                  uint32_t *id_out)
{
    uint32_t i;

    if (st == NULL || pk == NULL || id_out == NULL) {
        return -1;
    }
    for (i = 0; i < st->n_pool; i++) {
        if (memcmp(st->pool[i].pubkey, pk, 32) == 0) {
            *id_out = i;
            return 0;
        }
    }
    return -1;
}

int
compact_pool_mark_dirty(compact_state_t *st, uint32_t id)
{
    if (st == NULL || id >= st->n_pool) {
        return -1;
    }
    st->pool[id].grade = POOL_GRADE_DIRTY;
    return 0;
}

int
compact_token_find(const compact_state_t *st, const uint8_t pk[32],
                   uint32_t *id_out)
{
    uint32_t i;

    if (st == NULL || pk == NULL || id_out == NULL) {
        return -1;
    }
    for (i = 0; i < st->n_token; i++) {
        if (memcmp(st->token[i].pubkey, pk, 32) == 0) {
            *id_out = i;
            return 0;
        }
    }
    return -1;
}

int
compact_sys_find(const compact_state_t *st, const uint8_t pk[32],
                 uint32_t *id_out)
{
    uint32_t i;

    if (st == NULL || pk == NULL || id_out == NULL) {
        return -1;
    }
    for (i = 0; i < st->n_sys; i++) {
        if (memcmp(st->sys[i].pubkey, pk, 32) == 0) {
            *id_out = i;
            return 0;
        }
    }
    return -1;
}
