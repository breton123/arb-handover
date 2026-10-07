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

static uint32_t
pack_vec_tx(uint8_t *dst, const uint8_t *tx, uint32_t n)
{
    uint64_t nent = 1;

    memset(dst, 0, 8u + 48u + n);
    memcpy(dst, &nent, 8);
    dst[8 + 40] = 1;
    memcpy(dst + 8 + 48, tx, n);
    return 8u + 48u + n;
}

int
main(void)
{
    shred_assem_t *a = NULL;
    shred_batch_t b;
    uint8_t tx[256], vec[512], p0[600], p1[600], pool[32];
    uint32_t n, vn, mid;
    uint16_t l0, l1;
    const shred_stream_stats_t *st;

    memset(pool, 0x44, 32);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 77);
    CHECK(n > 40, "tx");
    vn = pack_vec_tx(vec, tx, n);
    mid = vn / 2u;
    l0 = wrap_part(p0, 9, 0, 0, vec, (uint16_t)mid);
    l1 = wrap_part(p1, 9, 1, 1, vec + mid, (uint16_t)(vn - mid));
    CHECK(shred_assem_init(&a) == 0, "init");
    CHECK(shred_assem_push(a, p0, l0, 1000, &b) == 0, "first wait");
    CHECK(shred_assem_push(a, p1, l1, 1400, &b) == 1, "multi complete");
    CHECK(b.n_shreds == 2, "multi shreds");
    CHECK(b.n_tx >= 1, "extracted tx");
    CHECK(b.tsc_first == 1000 && b.tsc_done == 1400, "clocks");
    CHECK(b.tx_len[0] == n, "tx len");
    shred_assem_free(a);

    CHECK(shred_assem_init(&a) == 0, "init2");
    l1 = wrap_part(p1, 3, 0, 1, vec, (uint16_t)vn);
    CHECK(shred_assem_push(a, p1, l1, 50, &b) == 1, "one packet");
    CHECK(b.how == RECON_ONE && b.n_shreds == 1, "one bucket");
    shred_assem_free(a);

    {
        uint8_t two[512], pkt0[400], pkt1[400];
        uint64_t nent = 2;
        uint16_t la, lb;

        memset(two, 0, sizeof(two));
        memcpy(two, &nent, 8);
        two[8 + 40] = 0;
        two[8 + 48 + 40] = 1;
        memcpy(two + 8 + 48 + 48, tx, n);
        la = wrap_part(pkt0, 7, 0, 1, two, 8 + 48);
        lb = wrap_part(pkt1, 7, 1, 1, two + 8 + 48,
                       (uint16_t)(48u + n));
        CHECK(shred_assem_init(&a) == 0, "init leftover");
        CHECK(shred_assem_push(a, pkt0, la, 10, &b) == 1, "tick complete");
        CHECK(b.n_tx == 0, "tick no tx");
        CHECK(shred_assem_push(a, pkt1, lb, 20, &b) == 1, "carry complete");
        CHECK(b.n_tx >= 1 && b.tx_len[0] == n, "leftover framed");
        shred_assem_free(a);
    }

    {
        uint8_t g0[400], g1[400], g2[400];
        uint16_t ga, gb, gc;

        ga = wrap_part(g0, 8, 0, 0, vec, (uint16_t)(vn / 3u));
        gb = wrap_part(g1, 8, 2, 1, vec + (2u * vn / 3u),
                       (uint16_t)(vn - (2u * vn / 3u)));
        gc = wrap_part(g2, 8, 1, 0, vec + (vn / 3u),
                       (uint16_t)((2u * vn / 3u) - (vn / 3u)));
        CHECK(shred_assem_init(&a) == 0, "init gap");
        CHECK(shred_assem_push(a, g0, ga, 1, &b) == 0, "idx0");
        CHECK(shred_assem_push(a, g1, gb, 2, &b) == 0, "idx2 blocked");
        CHECK(shred_assem_incomplete(a) == 1, "gap held");
        CHECK(shred_assem_push(a, g2, gc, 3, &b) == 1, "idx1 fills");
        CHECK(b.n_tx >= 1, "after gap");
        st = shred_assem_stream_stats(a);
        CHECK(st != NULL && st->gap_blocked >= 1, "gap metric");
        shred_assem_free(a);
    }

    CHECK(shred_assem_init(&a) == 0, "init3");
    l0 = wrap_part(p0, 4, 0, 0, vec, 4);
    CHECK(shred_assem_push(a, p0, l0, 1, &b) == 0, "orphan wait");
    CHECK(shred_assem_incomplete(a) == 0, "idx0 promoted");
    st = shred_assem_stream_stats(a);
    CHECK(st != NULL && st->leftover_bytes >= 4, "leftover tail");
    shred_assem_free(a);

    if (g_fail) {
        return 1;
    }
    printf("\nassemble ok\n");
    return 0;
}
