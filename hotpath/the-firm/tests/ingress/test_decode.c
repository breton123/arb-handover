#include "ingress/decode.h"
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
    uint8_t tx[512];
    uint8_t pool[32];
    ordered_tx_t o;
    uint32_t n;

    memset(pool, 0xab, 32);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1000);
    CHECK(n > 80, "encode");
    CHECK(ingress_decode_tx(tx, n, 9, &o) == DEC_OK, "decode ok");
    CHECK(o.n_ix == 1 && o.ix[0].proto == PROTO_PUMP, "pump proto");
    CHECK(o.ix[0].kind == IX_KIND_PUMP_SELL, "sell disc");
    CHECK(o.ix[0].amount_in == 1000, "amount");
    CHECK(o.n_keys == 2 && memcmp(o.key[0], pool, 32) == 0, "pool key");
    CHECK(o.slot == 9, "slot");

    CHECK(ingress_decode_tx(tx, 10, 1, &o) == DEC_INCOMPLETE, "short");
    if (g_fail) {
        return 1;
    }
    printf("\ndecode ok\n");
    return 0;
}
