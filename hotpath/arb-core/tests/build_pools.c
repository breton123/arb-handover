#include "arbrx.h"
#include "classify.h"
#include "pool.h"
#include "rx.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static classify_fn pick_classify(void)
{
    if (classify_have_avx2()) {
        return classify_avx2;
    }
    return classify_scalar;
}

int main(int argc, char **argv)
{
    const char *src;
    const char *dst;
    uint8_t *buf = NULL;
    size_t buflen = 0;
    arbrx_pkt_t *pkts = NULL;
    uint32_t npkts = 0;
    uint32_t i;
    pool_table_t tab;
    classify_fn cls;
    uint64_t relevant = 0;
    uint64_t named = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s shreds.arbrx pools.bin\n", argv[0]);
        return 1;
    }
    src = argv[1];
    dst = argv[2];
    if (arbrx_load(src, &buf, &buflen) != 0 || arbrx_index(buf, buflen, &pkts, &npkts) != 0) {
        fprintf(stderr, "failed to load %s\n", src);
        return 1;
    }
    if (pool_table_init(&tab, 1024) != 0) {
        return 1;
    }
    cls = pick_classify();
    for (i = 0; i < npkts; i++) {
        rx_packet_t pkt;
        shred_view_t view;
        uint16_t plen;
        uint32_t hit;
        msg_keys_t mk;
        pool_cand_t cand;
        uint16_t prog_off;
        uint8_t proto;

        pkt.data = pkts[i].data;
        pkt.len = pkts[i].len;
        pkt.queue = 0;
        if (hot_rx(&pkt, &view) != 0) {
            continue;
        }
        plen = (uint16_t)(pkt.len - (uint16_t)(view.payload - pkt.data));
        hit = cls(view.payload, plen);
        if (hit == REL_NONE) {
            continue;
        }
        relevant++;
        if (find_prog_id(view.payload, plen, &prog_off, &proto) != 0) {
            continue;
        }
        if (msg_keys_from_prog(view.payload, plen, prog_off, &mk) != 0) {
            continue;
        }
        if (msg_first_dex_pool(view.payload, plen, &mk, &cand) != 0) {
            continue;
        }
        if (pool_table_put(&tab, cand.key, cand.protocol) == 0) {
            named++;
        }
    }
    if (pool_table_save(&tab, dst) != 0) {
        pool_table_free(&tab);
        free(pkts);
        free(buf);
        return 1;
    }
    printf("shreds %u  relevant %" PRIu64 "  unique pools %u  new-this-pass %" PRIu64 "\n",
           npkts, relevant, tab.n, named);
    printf("wrote %s\n", dst);
    pool_table_free(&tab);
    free(pkts);
    free(buf);
    return 0;
}
