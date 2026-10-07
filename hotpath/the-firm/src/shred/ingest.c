#include "shred/ingest.h"

int
shred_ingest_packet(shred_race_t *race, shred_prefix_t *px,
                    const net_packet_t *pkt, shred_race_result_t *out)
{
    int rc;

    rc = shred_race_claim(race, pkt, out);
    if (rc == SHRED_RACE_WIN && px != NULL && pkt != NULL) {
        (void)shred_prefix_push(px, pkt->data, pkt->len);
    }
    return rc;
}
