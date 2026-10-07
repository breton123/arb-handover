#include "wire/classify.h"
#include "wire/mkpkt.h"

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
    uint8_t tx[256], pool[32];
    uint32_t n;
    wire_class_t cl;
    wire_lut_tab_t tab;

    memset(pool, 0x44, 32);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 9);
    CHECK(n > 40, "tx");
    CHECK(wire_classify(tx, n, NULL, &cl) == 0, "classify");
    CHECK(cl.klass == WIRE_CL_DIRECT_PUMP, "direct pump");

    wire_lut_tab_clear(&tab);
    CHECK(wire_lut_put(&tab, pool, pool, 1) == 0, "lut put");
    CHECK(tab.n == 1, "lut n");

    if (g_fail) {
        return 1;
    }
    printf("\nclassify ok\n");
    return 0;
}
