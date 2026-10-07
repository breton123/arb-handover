#include "shred/assemble.h"
#include "wire/mkpkt.h"

#include <stdio.h>
#include <stdlib.h>
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

static uint16_t
wrap_part(uint8_t *pkt, uint64_t slot, uint32_t index, uint8_t complete,
          const uint8_t *p, uint16_t n)
{
    memset(pkt, 0, SHRED_DATA_HDR_SZ + n);
    pkt[SHRED_OFF_VARIANT] = SHRED_TYPE_MERKLE_DATA;
    memcpy(pkt + SHRED_OFF_SLOT, &slot, 8);
    memcpy(pkt + SHRED_OFF_INDEX, &index, 4);
    pkt[85] = complete ? 0x80u : 0;
    {
        uint16_t tot = (uint16_t)(SHRED_DATA_HDR_SZ + n);
        memcpy(pkt + 86, &tot, 2);
    }
    memcpy(pkt + SHRED_DATA_HDR_SZ, p, n);
    return (uint16_t)(SHRED_DATA_HDR_SZ + n);
}

int
main(void)
{
    shred_assem_t *a = NULL;
    shred_batch_t b;
    uint8_t tx[256], p0[400], p1[400], pool[32];
    uint32_t n, mid;
    uint16_t l0, l1;

    memset(pool, 0x44, 32);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 77);
    CHECK(n > 40, "tx");
    mid = n / 2u;
    l0 = wrap_part(p0, 9, 10, 0, tx, (uint16_t)mid);
    l1 = wrap_part(p1, 9, 11, 1, tx + mid, (uint16_t)(n - mid));
    CHECK(shred_assem_init(&a) == 0, "init");
    CHECK(shred_assem_push(a, p0, l0, 1000, &b) == 0, "first wait");
    CHECK(shred_assem_push(a, p1, l1, 1400, &b) == 1, "multi complete");
    CHECK(b.how == RECON_MULTI && b.n_shreds == 2, "multi bucket");
    CHECK(b.n_tx >= 1, "extracted tx");
    CHECK(b.tsc_first == 1000 && b.tsc_done == 1400, "clocks");
    CHECK(b.tx_len[0] == n, "tx len");
    shred_assem_free(a);

    CHECK(shred_assem_init(&a) == 0, "init2");
    l1 = wrap_part(p1, 3, 0, 1, tx, (uint16_t)n);
    CHECK(shred_assem_push(a, p1, l1, 50, &b) == 1, "one packet");
    CHECK(b.how == RECON_ONE && b.n_shreds == 1, "one bucket");
    shred_assem_free(a);

    {
        uint8_t ticktx[400], pkt[500];
        uint16_t lp;

        memset(ticktx, 0, 48);
        ticktx[40] = 1;
        memcpy(ticktx + 48, tx, n);
        lp = wrap_part(pkt, 4, 0, 1, ticktx, (uint16_t)(48 + n));
        CHECK(shred_assem_init(&a) == 0, "init tick");
        CHECK(shred_assem_push(a, pkt, lp, 9, &b) == 1, "tick+tx");
        CHECK(b.n_tx >= 1 && b.tx_len[0] == n, "skip tick entry");
        shred_assem_free(a);
    }

    CHECK(shred_assem_init(&a) == 0, "init3");
    CHECK(shred_assem_push(a, p0, l0, 1, &b) == 0, "orphan wait");
    CHECK(shred_assem_incomplete(a) == 1, "incomplete bucket");
    shred_assem_free(a);

    if (g_fail) {
        return 1;
    }
    printf("\nassemble ok\n");
    return 0;
}
