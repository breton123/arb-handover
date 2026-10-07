#ifndef FIRM_STATE_CERT_H
#define FIRM_STATE_CERT_H

#include "ingress/tx.h"

#include <stdint.h>

/*
 * Publication certificate. Every published successor records why it
 * was considered exact. Filled only on successful apply (including
 * IRRELEVANT bank advance). Reject/abort leave the previous cert.
 */

#define STATE_MODEL_VERSION  17ull
#define STATE_PROGRAM_HASH   0x70616d6d2d303137ull /* "pamm-017" */

typedef struct {
    uint64_t slot;
    uint32_t tx_index;
    uint8_t  sig[STATE_SIG_LEN];
    uint64_t base_generation;
    uint64_t model_version;
    uint64_t program_hash;
    uint8_t  exactness_class;
    uint64_t dependency_hash;
} state_cert_t;

void state_cert_clear(state_cert_t *c);

uint64_t state_dep_hash(const ordered_tx_t *tx, const uint8_t *ix_class,
                        uint16_t n_ix);

void state_cert_fill(state_cert_t *c, uint64_t base_generation,
                     const ordered_tx_t *tx, uint8_t exactness_class,
                     uint64_t dependency_hash);

#endif /* FIRM_STATE_CERT_H */
