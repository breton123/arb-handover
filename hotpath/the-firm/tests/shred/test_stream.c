#include "shred/stream.h"
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
    uint8_t tx[256], buf[512];
    uint8_t pool[32];
    uint32_t n;
    uint64_t nent = 1;
    shred_stream_hit_t hit;
    shred_slot_stream_t ss;
    shred_stream_stats_t st;

    memset(pool, 0x44, 32);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 77);
    CHECK(n > 40, "tx");

    memset(buf, 0, sizeof(buf));
    memcpy(buf, &nent, 8);
    buf[8 + 40] = 1;
    memcpy(buf + 8 + 48, tx, n);
    CHECK(shred_stream_parse(buf, 8u + 48u + n, 9, &hit) == 0, "vec parse");
    CHECK(hit.n_ent == 1 && hit.n_tx >= 1, "vec entry+tx");
    CHECK(hit.tx_len[0] == n, "vec tx len");
    CHECK(hit.n_pump >= 1, "pump");

    memset(&st, 0, sizeof(st));
    CHECK(shred_slot_stream_init(&ss) == 0, "ss init");
    CHECK(shred_slot_stream_append(&ss, buf, 8 + 20, 1, 0, &st) == 0,
          "partial");
    shred_slot_stream_parse(&ss, 1, &st);
    CHECK(st.txs == 0, "incomplete tail");
    CHECK(shred_slot_stream_leftover(&ss) > 0, "leftover");
    CHECK(shred_slot_stream_append(&ss, buf + 8 + 20,
                                   (uint16_t)(28u + n), 2, 0, &st)
          == 0,
          "rest");
    shred_slot_stream_parse(&ss, 1, &st);
    CHECK(st.txs >= 1 && st.entries_incremental >= 1, "incremental");
    shred_slot_stream_free(&ss);

    if (g_fail) {
        return 1;
    }
    printf("\nstream ok\n");
    return 0;
}
