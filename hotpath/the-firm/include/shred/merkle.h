#ifndef FIRM_SHRED_MERKLE_H
#define FIRM_SHRED_MERKLE_H

#include <stdint.h>

#define SHRED_MERKLE_PROOF_B 20u
#define SHRED_MERKLE_ROOT_B  32u

void shred_merkle_leaf(const uint8_t *bytes, uint32_t n, uint8_t out[32]);
void shred_merkle_join(const uint8_t a[32], const uint8_t b[32], uint8_t out[32]);

/* Canonical tree root over n full 32-byte leaves (leaf hashes). */
int shred_merkle_root(const uint8_t *leaves, uint32_t n, uint8_t root[32]);

/* Fold proof (20-byte siblings) onto a leaf hash. */
int shred_merkle_root_from_proof(uint32_t index, const uint8_t leaf[32],
                                 const uint8_t *proof, uint32_t n_proof,
                                 uint8_t root[32]);

#endif
