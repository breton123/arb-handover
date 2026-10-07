#include "net/packet.h"
#include "net/time.h"
#include "shred/race.h"
#include "shred/shred.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

static uint64_t
percentile(uint64_t *v, size_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    return v[(size_t)(p * (double)(n - 1))];
}

static void
fill_shred(uint8_t *p, uint64_t slot, uint32_t index)
{
    uint16_t ver = 1;
    uint32_t fec = 1;
    memset(p, 0, 200);
    p[SHRED_OFF_VARIANT] = SHRED_TYPE_CHAINED_DATA;
    memcpy(p + SHRED_OFF_SLOT, &slot, 8);
    memcpy(p + SHRED_OFF_INDEX, &index, 4);
    memcpy(p + SHRED_OFF_VERSION, &ver, 2);
    memcpy(p + SHRED_OFF_FEC, &fec, 4);
}

int
main(void)
{
    const uint32_t count = 200000;
    const uint32_t warmup = 1000;
    shred_race_t *tab = NULL;
    net_tsc_clock_t tsc;
    net_packet_t pkt;
    shred_view_t view;
    shred_identity_t id;
    shred_race_result_t r;
    uint8_t buf[200];
    uint64_t *parse_cyc;
    uint64_t *claim_cyc;
    uint32_t i, n;
    uint64_t t0, t1;
    shred_race_stats_t st;

    if (net_tsc_calibrate(&tsc) != 0 ||
        shred_race_init(&tab, SHRED_RACE_DEFAULT_CAP) != 0) {
        return 1;
    }
    parse_cyc = calloc(count, sizeof(*parse_cyc));
    claim_cyc = calloc(count, sizeof(*claim_cyc));
    if (parse_cyc == NULL || claim_cyc == NULL) {
        shred_race_free(tab);
        free(parse_cyc);
        free(claim_cyc);
        return 1;
    }

    pkt.data = buf;
    pkt.len = 200;
    pkt.source_id = NET_SOURCE_ORBITFLARE;
    pkt.flags = 0;
    n = 0;
    for (i = 0; i < count + warmup; i++) {
        fill_shred(buf, 1000, i / 2u);
        pkt.seq = i;
        pkt.rx_ns = 1000ull + (uint64_t)i * 50ull;
        pkt.source_id = ((i % 2u) == 0u) ? NET_SOURCE_ORBITFLARE
                                         : NET_SOURCE_DOUBLEZERO;
        t0 = net_rdtscp();
        (void)shred_identify(buf, 200, &view, &id);
        t1 = net_rdtscp();
        if (i >= warmup && n < count) {
            parse_cyc[n] = t1 - t0;
        }
        t0 = net_rdtscp();
        (void)shred_race_claim(tab, &pkt, &r);
        t1 = net_rdtscp();
        if (i >= warmup && n < count) {
            claim_cyc[n] = t1 - t0;
            n++;
        }
    }

    shred_race_stats(tab, &st);
    printf("SHRED BENCH  parse + keyed claim\n");
    printf("count=%u  tsc_hz=%.6e  first_of=%" PRIu64 " first_dz=%" PRIu64
           " late_dz=%" PRIu64 "\n\n",
           count, tsc.hz, st.first[NET_SOURCE_ORBITFLARE],
           st.first[NET_SOURCE_DOUBLEZERO], st.late[NET_SOURCE_DOUBLEZERO]);

    for (i = 0; i < n; i++) {
        parse_cyc[i] = net_tsc_to_ns(&tsc, parse_cyc[i]);
        claim_cyc[i] = net_tsc_to_ns(&tsc, claim_cyc[i]);
    }
    qsort(parse_cyc, n, sizeof(*parse_cyc), cmp_u64);
    qsort(claim_cyc, n, sizeof(*claim_cyc), cmp_u64);
    printf("identify (header only)\n");
    printf("  p50  %" PRIu64 " ns\n", percentile(parse_cyc, n, 0.50));
    printf("  p99  %" PRIu64 " ns\n", percentile(parse_cyc, n, 0.99));
    printf("  max  %" PRIu64 " ns\n\n", parse_cyc[n - 1]);
    printf("claim (win or dup)\n");
    printf("  p50  %" PRIu64 " ns\n", percentile(claim_cyc, n, 0.50));
    printf("  p99  %" PRIu64 " ns\n", percentile(claim_cyc, n, 0.99));
    printf("  max  %" PRIu64 " ns\n", claim_cyc[n - 1]);
    printf("\nintra-process rdtscp, not NIC latency.\n");

    free(parse_cyc);
    free(claim_cyc);
    shred_race_free(tab);
    return 0;
}
