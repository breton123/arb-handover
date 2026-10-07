#include "shred/merkle.h"
#include "shred/sha256.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(c, m)                       \
    do {                                  \
        if (!(c)) {                       \
            fprintf(stderr, "FAIL  %s\n", (m)); \
            g_fail++;                     \
        } else {                          \
            printf("ok    %s\n", (m));    \
        }                                 \
    } while (0)

int
main(void)
{
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t h[32], leaves[2][32], root[32], proof_root[32], proof[20];

    shred_sha256((const uint8_t *)"abc", 3, h);
    CHECK(memcmp(h, abc, 32) == 0, "sha256 abc");
    shred_merkle_leaf((const uint8_t *)"L0", 2, leaves[0]);
    shred_merkle_leaf((const uint8_t *)"L1", 2, leaves[1]);
    CHECK(shred_merkle_root(&leaves[0][0], 2, root) == 0, "root2");
    memcpy(proof, leaves[1], 20);
    CHECK(shred_merkle_root_from_proof(0, leaves[0], proof, 1, proof_root) == 0,
          "proof0");
    CHECK(memcmp(root, proof_root, 32) == 0, "proof matches root");
    if (g_fail) {
        return 1;
    }
    printf("\nmerkle ok\n");
    return 0;
}
