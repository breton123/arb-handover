#include "tx/resolve.h"
#include "tx/view.h"
#include "wire/lut_cache.h"
#include "wire/lut_parse.h"

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

static void
pk(uint8_t *d, uint8_t v)
{
    memset(d, v, 32);
}

int
main(void)
{
    static lut_cache_t c;
    lut_account_t acc;
    uint8_t table[32], a0[32], a1[32], got[32], raw[56 + 64];
    tx_view_t v;

    pk(table, 0x11);
    pk(a0, 0xa0);
    pk(a1, 0xa1);
    lut_cache_clear(&c);

    CHECK(lut_cache_lookup(&c, table, 0, LUT_SLOT_NEVER, got) != 0, "empty miss");
    CHECK(lut_cache_learn_sparse(&c, table, 0, a0, NULL) == 0, "learn 0");
    CHECK(lut_cache_lookup(&c, table, 0, LUT_SLOT_NEVER, got) == 0
              && memcmp(got, a0, 32) == 0,
          "sparse hit");
    CHECK(lut_cache_learn_sparse(&c, table, 0, a1, NULL) != 0, "conflict");
    CHECK(c.n_quar == 1, "quarantine");
    CHECK(lut_cache_lookup(&c, table, 0, LUT_SLOT_NEVER, got) != 0,
          "quar miss");

    lut_cache_clear(&c);
    memset(&acc, 0, sizeof(acc));
    acc.n = 2;
    acc.have_meta = 1;
    acc.deactivation_slot = LUT_SLOT_NEVER;
    acc.last_extended_slot = 50;
    acc.start_index = 1;
    memcpy(acc.addr[0], a0, 32);
    memcpy(acc.addr[1], a1, 32);
    CHECK(lut_cache_install_account(&c, table, &acc) == 0, "install");
    CHECK(lut_cache_lookup(&c, table, 0, 50, got) == 0, "idx0 in extend slot");
    CHECK(lut_cache_lookup(&c, table, 1, 50, got) != 0, "new idx blocked");
    CHECK(lut_cache_lookup(&c, table, 1, 51, got) == 0, "next slot ok");

    memset(raw, 0, sizeof(raw));
    raw[0] = 1; /* ProgramState LookupTable */
    memset(raw + 4, 0xff, 8); /* never deactivate */
    raw[56] = 0xbb;
    CHECK(lut_parse_account(raw, (uint32_t)sizeof(raw), &acc) == 0, "parse");
    CHECK(acc.n == 2 && acc.addr[0][0] == 0xbb, "parse addrs");
    lut_cache_clear(&c);
    pk(table, 0x33);
    memcpy(c.miss[0], table, 32);
    c.n_miss = 1;
    CHECK(lut_cache_install_account(&c, table, &acc) == 0, "install for prune");
    lut_cache_prune_miss(&c);
    CHECK(c.n_miss == 0, "prune learned");

    memset(&v, 0, sizeof(v));
    v.n_static = 1;
    v.n_alt = 2;
    pk(v.alt[0].table, 0x22);
    pk(v.alt[1].table, 0x22); /* repeated descriptor */
    v.alt[0].nw = 1;
    v.alt[0].widx[0] = 0;
    v.alt[1].nw = 1;
    v.alt[1].widx[0] = 1;
    lut_cache_clear(&c);
    pk(table, 0x22);
    memcpy(acc.addr[0], a0, 32);
    memcpy(acc.addr[1], a1, 32);
    acc.n = 2;
    acc.have_meta = 0;
    CHECK(lut_cache_install_account(&c, table, &acc) == 0, "rep table");
    CHECK(txview_resolve_at(&v, &c, LUT_SLOT_NEVER) == TXV_RES_OK, "repeat desc");
    CHECK(v.n_keys == 3, "static+2 loaded");
    CHECK(memcmp(v.key[1], a0, 32) == 0 && memcmp(v.key[2], a1, 32) == 0,
          "canonical W then W");

    if (g_fail) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nlut cache ok\n");
    return 0;
}
