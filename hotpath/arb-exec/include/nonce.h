#ifndef ARB_EXEC_NONCE_H
#define ARB_EXEC_NONCE_H

#include <stdint.h>
#include <string.h>

/*
 * EXEC-004 — durable nonce pool.
 *
 * One exec thread owns the ring. No locks. RPC refresh is load/retire
 * on the control plane. Hot path only claims a READY hash.
 *
 *   attempt A → nonce i
 *   attempt B → nonce i+1
 *   attempt C → nonce i+2
 */

#define NONCE_POOL_N  64u

typedef enum {
    NONCE_READY = 0,
    NONCE_IN_FLIGHT = 1,
    NONCE_REFRESH = 2
} nonce_state_t;

typedef struct {
    uint8_t       pubkey[32];
    uint8_t       hash[32];
    nonce_state_t state;
} nonce_slot_t;

typedef struct {
    nonce_slot_t slot[NONCE_POOL_N];
    uint32_t     next;
    uint32_t     n_ready;
} nonce_pool_t;

typedef struct {
    uint16_t idx;
    uint8_t  hash[32];
} nonce_claim_t;

void nonce_pool_init(nonce_pool_t *p);

int nonce_load(nonce_pool_t *p, uint32_t idx,
               const uint8_t pubkey[32], const uint8_t hash[32]);

int nonce_retire(nonce_pool_t *p, uint32_t idx);

int nonce_reload(nonce_pool_t *p, uint32_t idx, const uint8_t hash[32]);

static inline int
nonce_claim(nonce_pool_t *p, nonce_claim_t *out)
{
    uint32_t i;
    uint32_t n;

    if (p == NULL || out == NULL || p->n_ready == 0u) {
        return -1;
    }
    i = p->next;
    for (n = 0; n < NONCE_POOL_N; n++) {
        nonce_slot_t *s = &p->slot[i];
        if (s->state == NONCE_READY) {
            memcpy(out->hash, s->hash, 32);
            out->idx = (uint16_t)i;
            s->state = NONCE_IN_FLIGHT;
            p->n_ready--;
            p->next = (i + 1u < NONCE_POOL_N) ? (i + 1u) : 0u;
            return 0;
        }
        i = (i + 1u < NONCE_POOL_N) ? (i + 1u) : 0u;
    }
    return -1;
}

#endif /* ARB_EXEC_NONCE_H */
