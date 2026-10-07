#ifndef FIRM_SHRED_RACE_H
#define FIRM_SHRED_RACE_H

#include "net/packet.h"
#include "shred/shred.h"

#include <stdint.h>

/*
 * First-writer-wins keyed race.
 *
 * Claim as soon as a packet is in hand. Do not wait for other feeds.
 * The first valid identity proceeds downstream; later matches only
 * update telemetry (delta, % first).
 *
 * One writer (the race/sink thread). Startup allocation only.
 */

#define SHRED_RACE_WIN  0
#define SHRED_RACE_DUP  1
#define SHRED_RACE_BAD  2

#define SHRED_RACE_DEFAULT_CAP   (1u << 17)
#define SHRED_RACE_SAMPLE_CAP    65536u
#define SHRED_RACE_PROBE         16u
#define SHRED_RACE_SLOT_LAG      64u

typedef struct {
    int              verdict;
    uint8_t          first_source;
    uint8_t          late_source;
    int64_t          delta_ns;
    int64_t          delta_tsc;
    shred_identity_t id;
    shred_view_t     view;
} shred_race_result_t;

typedef struct {
    uint64_t first[NET_SOURCE_MAX];
    uint64_t late[NET_SOURCE_MAX];
    uint64_t bad;
    uint64_t evict;
    uint64_t pair_n[NET_SOURCE_MAX][NET_SOURCE_MAX];
    int64_t  pair_min[NET_SOURCE_MAX][NET_SOURCE_MAX];
    int64_t  pair_max[NET_SOURCE_MAX][NET_SOURCE_MAX];
} shred_race_stats_t;

typedef struct shred_race shred_race_t;

int shred_race_init(shred_race_t **out, uint32_t cap);
void shred_race_free(shred_race_t *t);

int shred_race_claim(shred_race_t *t, const net_packet_t *pkt,
                     shred_race_result_t *out);

void shred_race_stats(const shred_race_t *t, shred_race_stats_t *out);

/*
 * Cold path. Copies matching samples and computes min/p50/p95/max.
 * n_out is the total pair count (may exceed the sample cap).
 */
int shred_race_pair_pct(const shred_race_t *t, uint8_t first, uint8_t late,
                        uint64_t *n_out, int64_t *min_ns, int64_t *p50_ns,
                        int64_t *p95_ns, int64_t *p99_ns, int64_t *max_ns);

#endif /* FIRM_SHRED_RACE_H */
