#ifndef FIRM_SHRED_INGEST_H
#define FIRM_SHRED_INGEST_H

#include "shred/prefix.h"
#include "shred/race.h"

/*
 * net_packet_t → envelope → identity → first-writer claim.
 * WIN is forwarded to the prefix assembler immediately.
 * DUP is telemetry only. No wait window.
 */

int shred_ingest_packet(shred_race_t *race, shred_prefix_t *px,
                        const net_packet_t *pkt, shred_race_result_t *out);

#endif
