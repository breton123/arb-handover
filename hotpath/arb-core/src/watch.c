#include "watch.h"

#include <stdlib.h>
#include <string.h>

static uint32_t
h32(const uint8_t pk[32], uint32_t cap)
{
    uint32_t x;
    memcpy(&x, pk, 4);
    return x % cap;
}

int
watch_init_cap(watch_idx_t *w, uint32_t cap)
{
    if (w == NULL || cap < 8u) {
        return -1;
    }
    memset(w, 0, sizeof(*w));
    w->cap = cap;
    w->ent = calloc(w->cap, sizeof(*w->ent));
    return w->ent == NULL ? -1 : 0;
}

int
watch_init(watch_idx_t *w)
{
    return watch_init_cap(w, WATCH_CAP);
}

void
watch_free(watch_idx_t *w)
{
    if (w == NULL) {
        return;
    }
    free(w->ent);
    memset(w, 0, sizeof(*w));
}

int
watch_put(watch_idx_t *w, const uint8_t pk[32], uint32_t pool_idx,
          uint8_t protocol, uint8_t role)
{
    uint32_t i, s;
    if (w == NULL || pk == NULL) {
        return -1;
    }
    s = h32(pk, w->cap);
    for (i = 0; i < w->cap; i++) {
        watch_ent_t *e = &w->ent[(s + i) % w->cap];
        if (e->used && memcmp(e->pk, pk, 32) == 0) {
            return 0;
        }
        if (!e->used) {
            e->used = 1;
            memcpy(e->pk, pk, 32);
            e->protocol = protocol;
            e->role = role;
            e->pool_idx = pool_idx;
            w->n++;
            return 0;
        }
    }
    return -1;
}

int
watch_get(const watch_idx_t *w, const uint8_t pk[32], watch_ent_t *out)
{
    uint32_t i, s;
    if (w == NULL || pk == NULL) {
        return -1;
    }
    s = h32(pk, w->cap);
    for (i = 0; i < w->cap; i++) {
        const watch_ent_t *e = &w->ent[(s + i) % w->cap];
        if (!e->used) {
            return -1;
        }
        if (memcmp(e->pk, pk, 32) == 0) {
            if (out != NULL) {
                *out = *e;
            }
            return 0;
        }
    }
    return -1;
}

int
watch_from_univ(watch_idx_t *w, const live_univ_t *u)
{
    uint32_t i;
    if (w == NULL || u == NULL) {
        return -1;
    }
    if (u->n * 4u > w->cap) {
        uint32_t need = u->n * 8u;
        watch_free(w);
        if (watch_init_cap(w, need < 4096u ? 4096u : need) != 0) {
            return -1;
        }
    }
    for (i = 0; i < u->n; i++) {
        const live_meta_t *m = &u->meta[i];
        (void)watch_put(w, m->pubkey, i, m->protocol, WATCH_ROLE_POOL);
        (void)watch_put(w, m->vault_x, i, m->protocol, WATCH_ROLE_VAULT);
        (void)watch_put(w, m->vault_y, i, m->protocol, WATCH_ROLE_VAULT);
    }
    return 0;
}
