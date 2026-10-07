#include "net/packet.h"
#include "shred/ingest.h"
#include "shred/prefix.h"
#include "shred/race.h"
#include "shred/shred.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL  %s\n", (msg)); \
            g_fail++;                             \
        } else {                                  \
            printf("ok    %s\n", (msg));          \
        }                                         \
    } while (0)

static void
fill_shred(uint8_t *p, uint16_t n, uint64_t slot, uint32_t index,
           uint32_t fec, uint8_t type)
{
    uint16_t ver = 1;
    memset(p, 0, n);
    p[SHRED_OFF_VARIANT] = type;
    memcpy(p + SHRED_OFF_SLOT, &slot, 8);
    memcpy(p + SHRED_OFF_INDEX, &index, 4);
    memcpy(p + SHRED_OFF_VERSION, &ver, 2);
    memcpy(p + SHRED_OFF_FEC, &fec, 4);
}

static void
mk_pkt(net_packet_t *p, uint8_t *buf, uint8_t src, uint64_t rx_ns,
       uint64_t slot, uint32_t index)
{
    fill_shred(buf, 200, slot, index, 1, SHRED_TYPE_CHAINED_DATA);
    p->data = buf;
    p->len = 200;
    p->source_id = src;
    p->flags = 0;
    p->seq = index;
    p->rx_ns = rx_ns;
    p->rx_tsc = rx_ns * 2u;
}

int
main(void)
{
    shred_race_t *tab = NULL;
    shred_race_result_t r;
    shred_race_stats_t st;
    net_packet_t pkt;
    uint8_t a[200], b[200], c[200];
    uint64_t n;
    int64_t mn, p50, p95, p99, mx;

    CHECK(shred_race_init(&tab, SHRED_RACE_DEFAULT_CAP) == 0, "table init");

    mk_pkt(&pkt, a, NET_SOURCE_ORBITFLARE, 1000, 50, 1);
    CHECK(shred_race_claim(tab, &pkt, &r) == SHRED_RACE_WIN, "first is win");
    CHECK(r.first_source == NET_SOURCE_ORBITFLARE, "winner source");
    CHECK(r.id.slot == 50 && r.id.index == 1, "identity");

    mk_pkt(&pkt, b, NET_SOURCE_DOUBLEZERO, 184000, 50, 1);
    memset(b, 0xAA, 64);
    b[SHRED_OFF_VARIANT] = SHRED_TYPE_CHAINED_DATA;
    {
        uint64_t slot = 50;
        uint32_t index = 1, fec = 1;
        uint16_t ver = 1;
        memcpy(b + SHRED_OFF_SLOT, &slot, 8);
        memcpy(b + SHRED_OFF_INDEX, &index, 4);
        memcpy(b + SHRED_OFF_VERSION, &ver, 2);
        memcpy(b + SHRED_OFF_FEC, &fec, 4);
    }
    pkt.data = b;
    pkt.len = 200;
    pkt.source_id = NET_SOURCE_DOUBLEZERO;
    pkt.rx_ns = 184000;
    CHECK(shred_race_claim(tab, &pkt, &r) == SHRED_RACE_DUP, "later copy is dup");
    CHECK(r.first_source == NET_SOURCE_ORBITFLARE, "dup remembers winner");
    CHECK(r.late_source == NET_SOURCE_DOUBLEZERO, "dup source");
    CHECK(r.delta_ns == 183000, "delta is late minus first");
    CHECK(r.delta_tsc == 183000 * 2, "tsc delta is late minus first");

    mk_pkt(&pkt, c, NET_SOURCE_BLOCKSPACE, 900, 50, 2);
    CHECK(shred_race_claim(tab, &pkt, &r) == SHRED_RACE_WIN,
          "different index is a different shred");
    CHECK(r.first_source == NET_SOURCE_BLOCKSPACE, "other shred can win");

    shred_race_stats(tab, &st);
    CHECK(st.first[NET_SOURCE_ORBITFLARE] == 1, "OF first count");
    CHECK(st.first[NET_SOURCE_BLOCKSPACE] == 1, "BS first count");
    CHECK(st.late[NET_SOURCE_DOUBLEZERO] == 1, "DZ late count");
    CHECK(st.pair_n[NET_SOURCE_ORBITFLARE][NET_SOURCE_DOUBLEZERO] == 1,
          "pair count");
    CHECK(shred_race_pair_pct(tab, NET_SOURCE_ORBITFLARE,
                              NET_SOURCE_DOUBLEZERO, &n, &mn, &p50, &p95,
                              &p99, &mx) == 0,
          "pair pct");
    CHECK(n == 1 && mn == 183000 && p50 == 183000 && p99 == 183000,
          "single-sample percentiles");

    pkt.data = a;
    pkt.len = 20;
    pkt.source_id = NET_SOURCE_ORBITFLARE;
    CHECK(shred_race_claim(tab, &pkt, &r) == SHRED_RACE_BAD, "bad envelope");
    shred_race_stats(tab, &st);
    CHECK(st.bad == 1, "bad counted");

    {
        shred_prefix_t *px = NULL;
        shred_prefix_info_t info;
        uint8_t wrapped[250];
        uint16_t ver9 = 9;

        CHECK(shred_prefix_init(&px) == 0 && shred_prefix_watch(px, 90) == 0,
              "prefix watch");
        mk_pkt(&pkt, a, NET_SOURCE_ORBITFLARE, 1, 90, 0);
        CHECK(shred_ingest_packet(tab, px, &pkt, &r) == SHRED_RACE_WIN,
              "ingest win forwards");
        mk_pkt(&pkt, b, NET_SOURCE_DOUBLEZERO, 40000, 90, 0);
        memcpy(b + SHRED_OFF_VERSION, &ver9, 2);
        CHECK(shred_ingest_packet(tab, px, &pkt, &r) == SHRED_RACE_DUP,
              "version is not part of the race key");
        CHECK(r.first_source == NET_SOURCE_ORBITFLARE, "dup names winner");
        CHECK(shred_prefix_info(px, 90, &info) == 0 && info.n_data == 1,
              "dup is not a second prefix row");

        memset(wrapped, 0, sizeof(wrapped));
        fill_shred(wrapped + 28, 200, 90, 3, 1, SHRED_TYPE_CHAINED_DATA);
        pkt.data = wrapped;
        pkt.len = 228;
        pkt.source_id = NET_SOURCE_ORBITFLARE;
        pkt.rx_ns = 5;
        pkt.rx_tsc = 10;
        CHECK(shred_ingest_packet(tab, px, &pkt, &r) == SHRED_RACE_WIN,
              "claim through IPv4/UDP header offset");
        CHECK(r.id.slot == 90 && r.id.index == 3, "envelope identity");
        CHECK(shred_prefix_info(px, 90, &info) == 0 && info.n_data == 2,
              "offset-28 winner still assembled");
        shred_prefix_free(px);
    }

    shred_race_free(tab);

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nshred_race ok\n");
    return 0;
}
