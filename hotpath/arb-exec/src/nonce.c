#include "nonce.h"

void
nonce_pool_init(nonce_pool_t *p)
{
    uint32_t i;

    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    for (i = 0; i < NONCE_POOL_N; i++) {
        p->slot[i].state = NONCE_REFRESH;
    }
}

int
nonce_load(nonce_pool_t *p, uint32_t idx,
           const uint8_t pubkey[32], const uint8_t hash[32])
{
    nonce_slot_t *s;

    if (p == NULL || pubkey == NULL || hash == NULL || idx >= NONCE_POOL_N) {
        return -1;
    }
    s = &p->slot[idx];
    if (s->state == NONCE_READY) {
        return -1;
    }
    memcpy(s->pubkey, pubkey, 32);
    memcpy(s->hash, hash, 32);
    if (s->state != NONCE_READY) {
        p->n_ready++;
    }
    s->state = NONCE_READY;
    return 0;
}

int
nonce_retire(nonce_pool_t *p, uint32_t idx)
{
    nonce_slot_t *s;

    if (p == NULL || idx >= NONCE_POOL_N) {
        return -1;
    }
    s = &p->slot[idx];
    if (s->state != NONCE_IN_FLIGHT) {
        return -1;
    }
    s->state = NONCE_REFRESH;
    return 0;
}

int
nonce_reload(nonce_pool_t *p, uint32_t idx, const uint8_t hash[32])
{
    nonce_slot_t *s;

    if (p == NULL || hash == NULL || idx >= NONCE_POOL_N) {
        return -1;
    }
    s = &p->slot[idx];
    if (s->state != NONCE_REFRESH && s->state != NONCE_IN_FLIGHT) {
        return -1;
    }
    memcpy(s->hash, hash, 32);
    if (s->state != NONCE_READY) {
        p->n_ready++;
    }
    s->state = NONCE_READY;
    return 0;
}
