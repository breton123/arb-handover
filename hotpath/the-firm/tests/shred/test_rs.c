#include "shred/rs.h"

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
    uint8_t d0[16], d1[16], d2[16], d3[16];
    uint8_t c0[16], c1[16], c2[16], c3[16];
    uint8_t r0[16], r1[16];
    uint8_t *data[4], *code[4], *all[8];
    uint8_t have[8];
    uint32_t i;

    for (i = 0; i < 16; i++) {
        d0[i] = (uint8_t)(i + 1);
        d1[i] = (uint8_t)(i + 17);
        d2[i] = (uint8_t)(i + 33);
        d3[i] = (uint8_t)(i + 49);
    }
    data[0] = d0;
    data[1] = d1;
    data[2] = d2;
    data[3] = d3;
    code[0] = c0;
    code[1] = c1;
    code[2] = c2;
    code[3] = c3;
    CHECK(shred_rs_encode(data, code, 4, 4, 16) == 0, "encode");
    memset(have, 1, 8);
    have[0] = 0;
    have[1] = 0;
    all[0] = r0;
    all[1] = r1;
    all[2] = d2;
    all[3] = d3;
    all[4] = c0;
    all[5] = c1;
    all[6] = c2;
    all[7] = c3;
    CHECK(shred_rs_recover(all, have, 4, 4, 16) == 0, "recover");
    CHECK(memcmp(r0, d0, 16) == 0 && memcmp(r1, d1, 16) == 0, "match");

    {
        uint8_t big[64][64];
        uint8_t *dd[32], *cc[32], *all32[64];
        uint8_t have32[64];
        uint32_t s, t;

        for (s = 0; s < 32; s++) {
            for (t = 0; t < 64; t++) {
                big[s][t] = (uint8_t)(s + t * 3u);
            }
            dd[s] = big[s];
            cc[s] = big[32 + s];
        }
        CHECK(shred_rs_encode(dd, cc, 32, 32, 64) == 0, "encode 32");
        memset(have32, 1, 64);
        have32[0] = 0;
        memset(big[0], 0, 64);
        for (s = 0; s < 64; s++) {
            all32[s] = big[s];
        }
        CHECK(shred_rs_recover(all32, have32, 32, 32, 64) == 0, "rec miss1");
        for (t = 0; t < 64; t++) {
            if (big[0][t] != (uint8_t)(t * 3u)) {
                g_fail++;
                fprintf(stderr, "FAIL  miss1 byte\n");
                break;
            }
        }
        if (t == 64) {
            printf("ok    miss1 match\n");
        }
        have32[0] = 0;
        have32[1] = 0;
        have32[2] = 0;
        have32[3] = 0;
        memset(big[0], 0, 64);
        memset(big[1], 0, 64);
        memset(big[2], 0, 64);
        memset(big[3], 0, 64);
        CHECK(shred_rs_recover(all32, have32, 32, 32, 64) == 0, "rec miss4");
        CHECK(big[0][0] == 0 && big[3][1] == (uint8_t)(3 + 3), "miss4 match");
    }
    if (g_fail) {
        return 1;
    }
    printf("\nrs ok\n");
    return 0;
}
