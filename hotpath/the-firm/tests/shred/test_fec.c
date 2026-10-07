#include "shred/assemble.h"
#include "shred/fec.h"
#include "shred/merkle.h"
#include "shred/rs.h"
#include "shred/shred.h"

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

#define N 4u
#define K 4u
#define PN 3u

static uint32_t
dcap(void)
{
    return 1203u - 88u - 32u - 20u * PN;
}

static void
fill_data(uint8_t *p, uint64_t slot, uint32_t idx, uint32_t fec, uint8_t flags)
{
    uint16_t size;
    uint32_t cap = dcap();

    memset(p, 0, 1203);
    memset(p, 0xAB, 64);
    p[64] = (uint8_t)(SHRED_TYPE_CHAINED_DATA | PN);
    memcpy(p + 65, &slot, 8);
    memcpy(p + 73, &idx, 4);
    p[77] = 1;
    memcpy(p + 79, &fec, 4);
    p[85] = flags;
    size = (uint16_t)(88u + cap);
    memcpy(p + 86, &size, 2);
    p[88] = (uint8_t)idx;
    p[89] = 0x5A;
}

static void
fill_code(uint8_t *p, uint64_t slot, uint32_t fec, uint16_t pos,
          const uint8_t *erasure)
{
    uint32_t idx = fec + pos;
    uint16_t nd = (uint16_t)N, nk = (uint16_t)K;
    uint32_t cap = 1228u - 89u - 32u - 20u * PN;

    memset(p, 0, 1228);
    memset(p, 0xAB, 64);
    p[64] = (uint8_t)(SHRED_TYPE_CHAINED_CODE | PN);
    memcpy(p + 65, &slot, 8);
    memcpy(p + 73, &idx, 4);
    p[77] = 1;
    memcpy(p + 79, &fec, 4);
    memcpy(p + 83, &nd, 2);
    memcpy(p + 85, &nk, 2);
    memcpy(p + 87, &pos, 2);
    memcpy(p + 89, erasure, cap);
}

int
main(void)
{
    uint8_t data[N][1203], code[K][1228];
    uint8_t *esh[N + K], have[N + K];
    uint8_t erasure[N + K][1100];
    uint8_t leaves[N + K][32], root[32];
    uint32_t cap = dcap(), elen = 24u + cap, i, j;
    uint8_t chained[32];
    shred_fec_t *f = NULL;
    shred_fec_rec_t rec[8];
    uint32_t nrec = 0;

    memset(chained, 0x11, 32);
    for (i = 0; i < N; i++) {
        fill_data(data[i], 9, i, 0, (uint8_t)(i + 1 == N ? 0x80 : 0));
        memcpy(data[i] + 88 + cap, chained, 32);
        memcpy(erasure[i], data[i] + 64, elen);
        esh[i] = erasure[i];
    }
    for (i = 0; i < K; i++) {
        esh[N + i] = erasure[N + i];
    }
    CHECK(shred_rs_encode(esh, &esh[N], N, K, elen) == 0, "rs encode");
    for (i = 0; i < K; i++) {
        fill_code(code[i], 9, 0, (uint16_t)i, erasure[N + i]);
        memcpy(code[i] + 89 + (1228u - 89u - 32u - 20u * PN), chained, 32);
    }
    for (i = 0; i < N; i++) {
        shred_merkle_leaf(data[i] + 64, 88u + cap + 32u - 64u, leaves[i]);
    }
    for (i = 0; i < K; i++) {
        uint32_t poff = 89u + (1228u - 89u - 32u - 20u * PN) + 32u;
        shred_merkle_leaf(code[i] + 64, poff - 64u, leaves[N + i]);
    }
    CHECK(shred_merkle_root(&leaves[0][0], N + K, root) == 0, "tree");
    for (i = 0; i < N + K; i++) {
        uint8_t *pkt = (i < N) ? data[i] : code[i - N];
        uint32_t poff = (i < N) ? (88u + cap + 32u)
                                : (89u + (1228u - 89u - 32u - 20u * PN) + 32u);
        uint32_t idx = i, sz = N + K, off = 0;
        uint8_t level[8][32];

        memcpy(level, leaves, sizeof(leaves));
        while (sz > 1) {
            uint32_t sib = (idx ^ 1u);
            if (sib >= sz) {
                sib = sz - 1u;
            }
            memcpy(pkt + poff + off, level[sib], 20);
            off += 20;
            {
                uint8_t nxt[8][32];
                uint32_t nn = 0, a;
                for (a = 0; a < sz; a += 2) {
                    uint32_t b = (a + 1u < sz) ? a + 1u : a;
                    shred_merkle_join(level[a], level[b], nxt[nn++]);
                }
                memcpy(level, nxt, sizeof(nxt));
                idx >>= 1;
                sz = nn;
            }
        }
        (void)j;
    }

    CHECK(shred_fec_init(&f) == 0, "fec init");
    memset(have, 0, sizeof(have));
    for (i = 2; i < N; i++) {
        int rc = shred_fec_push(f, data[i], 1203, 10 + i, rec, 8, &nrec);
        CHECK(rc == 0 && nrec == 0, "data wait");
    }
    {
        int got = 0;
        for (i = 0; i < K; i++) {
            int rc = shred_fec_push(f, code[i], 1228, 20 + i, rec, 8, &nrec);
            if (rc == 1 && nrec == 2) {
                CHECK(rec[0].index == 0 && rec[1].index == 1, "indices");
                CHECK(memcmp(rec[0].pkt + 88, data[0] + 88, 8) == 0,
                      "payload0");
                got = 1;
            }
        }
        CHECK(got == 1, "recovered two");
    }
    CHECK(shred_fec_stats(f)->recovered == 1, "recovered count");
    CHECK(shred_fec_stats(f)->auth_ok == 1, "auth ok");
    CHECK(shred_fec_stats(f)->auth_fail == 0, "auth fail 0");
    shred_fec_free(f);

    if (g_fail) {
        return 1;
    }
    printf("\nfec ok\n");
    return 0;
}
