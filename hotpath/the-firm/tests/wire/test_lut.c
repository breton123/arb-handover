#include "wire/lut_file.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
    char path[] = "/tmp/firm_lut_test.jsonl";
    FILE *f;
    wire_lut_tab_t tab;
    uint8_t key[32], addr[32];
    uint32_t i;

    for (i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
        addr[i] = (uint8_t)(i + 1);
    }
    f = fopen(path, "w");
    CHECK(f != NULL, "tmp");
    fprintf(f,
            "{\"key_hex\":\"000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f\","
            "\"addrs_hex\":[\"0102030405060708090a0b0c0d0e0f10"
            "1112131415161718191a1b1c1d1e1f20\"]}\n");
    fclose(f);
    wire_lut_tab_clear(&tab);
    CHECK(wire_lut_load_jsonl(&tab, path) == 0, "load");
    CHECK(tab.n == 1 && tab.t[0].n == 1, "one table");
    CHECK(memcmp(tab.t[0].key, key, 32) == 0, "key");
    CHECK(memcmp(tab.t[0].addr[0], addr, 32) == 0, "addr");
    (void)unlink(path);
    if (g_fail) {
        return 1;
    }
    printf("\nlut ok\n");
    return 0;
}
