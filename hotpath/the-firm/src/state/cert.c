#include "state/cert.h"

#include <stddef.h>
#include <string.h>

static uint64_t
fnv1a64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (uint64_t)b[i];
        h *= 1099511628211ull;
    }
    return h;
}

void
state_cert_clear(state_cert_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
}

uint64_t
state_dep_hash(const ordered_tx_t *tx, const uint8_t *ix_class, uint16_t n_ix)
{
    uint64_t h = 14695981039346656037ull;
    uint16_t i;

    if (tx == NULL) {
        return 0;
    }
    h = fnv1a64(h, &tx->slot, sizeof(tx->slot));
    h = fnv1a64(h, &tx->tx_index, sizeof(tx->tx_index));
    h = fnv1a64(h, tx->sig, STATE_SIG_LEN);
    for (i = 0; i < n_ix && i < tx->n_ix && i < STATE_IX_MAX; i++) {
        const ordered_ix_t *ix = &tx->ix[i];
        uint8_t c = (ix_class != NULL) ? ix_class[i] : 0;

        h = fnv1a64(h, &c, 1);
        h = fnv1a64(h, &ix->proto, 1);
        h = fnv1a64(h, &ix->kind, 1);
        h = fnv1a64(h, &ix->pool_id, sizeof(ix->pool_id));
        h = fnv1a64(h, &ix->src_token, sizeof(ix->src_token));
        h = fnv1a64(h, &ix->dst_token, sizeof(ix->dst_token));
        h = fnv1a64(h, &ix->amount_in, sizeof(ix->amount_in));
        h = fnv1a64(h, &ix->min_out, sizeof(ix->min_out));
    }
    return h;
}

void
state_cert_fill(state_cert_t *c, uint64_t base_generation,
                const ordered_tx_t *tx, uint8_t exactness_class,
                uint64_t dependency_hash)
{
    if (c == NULL) {
        return;
    }
    state_cert_clear(c);
    if (tx == NULL) {
        return;
    }
    c->slot = tx->slot;
    c->tx_index = tx->tx_index;
    memcpy(c->sig, tx->sig, STATE_SIG_LEN);
    c->base_generation = base_generation;
    c->model_version = STATE_MODEL_VERSION;
    c->program_hash = STATE_PROGRAM_HASH;
    c->exactness_class = exactness_class;
    c->dependency_hash = dependency_hash;
}
