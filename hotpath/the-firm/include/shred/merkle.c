#include "shred/merkle.h"

#include "shred/sha256.h"

#include <string.h>

static const uint8_t PRE_LEAF[27] = "\x00SOLANA_MERKLE_SHREDS_LEAF";
static const uint8_t PRE_NODE[27] = "\x01SOLANA_MERKLE_SHREDS_NODE";

void
shred_merkle_leaf(const uint8_t *bytes, uint32_t n, uint8_t out[32])
{
    uint8_t buf[26 + 2048];

    if (n > 2048) {
        memset(out, 0, 32);
        return;
    }
    memcpy(buf, PRE_LEAF, 26);
    memcpy(buf + 26, bytes, n);
    shred_sha256(buf, 26u + n, out);
}

void
shred_merkle_join(const uint8_t a[32], const uint8_t b[32], uint8_t out[32])
{
    uint8_t buf[26 + 20 + 20];

    memcpy(buf, PRE_NODE, 26);
    memcpy(buf + 26, a, SHRED_MERKLE_PROOF_B);
    memcpy(buf + 46, b, SHRED_MERKLE_PROOF_B);
    shred_sha256(buf, 66, out);
}

int
shred_merkle_root(const uint8_t *leaves, uint32_t n, uint8_t root[32])
{
    uint8_t cur[134][32];
    uint8_t nxt[134][32];
    uint32_t size, i;

    if (leaves == NULL || root == NULL || n == 0 || n > 134) {
        return -1;
    }
    memcpy(cur, leaves, (size_t)n * 32u);
    size = n;
    while (size > 1) {
        uint32_t nn = 0;
        for (i = 0; i < size; i += 2) {
            uint32_t j = (i + 1u < size) ? (i + 1u) : i;
            shred_merkle_join(cur[i], cur[j], nxt[nn]);
            nn++;
        }
        memcpy(cur, nxt, (size_t)nn * 32u);
        size = nn;
    }
    memcpy(root, cur[0], 32);
    return 0;
}

int
shred_merkle_root_from_proof(uint32_t index, const uint8_t leaf[32],
                             const uint8_t *proof, uint32_t n_proof,
                             uint8_t root[32])
{
    uint8_t node[32], sib[32];
    uint32_t i;

    if (leaf == NULL || root == NULL || (n_proof != 0 && proof == NULL)) {
        return -1;
    }
    memcpy(node, leaf, 32);
    memset(sib, 0, 32);
    for (i = 0; i < n_proof; i++) {
        memcpy(sib, proof + i * SHRED_MERKLE_PROOF_B, SHRED_MERKLE_PROOF_B);
        if ((index & 1u) == 0) {
            shred_merkle_join(node, sib, node);
        } else {
            shred_merkle_join(sib, node, node);
        }
        index >>= 1;
    }
    memcpy(root, node, 32);
    return 0;
}
