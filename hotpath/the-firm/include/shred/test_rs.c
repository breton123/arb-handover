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
    if (g_fail) {
        return 1;
    }
    printf("\nrs ok\n");
    return 0;
}
