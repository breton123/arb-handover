#ifndef ARB_CORE_TRIGGER_H
#define ARB_CORE_TRIGGER_H

#include "alt_cache.h"
#include "frame.h"
#include "swapix.h"
#include "watch.h"

#include <stdint.h>

/*
 * TRIGGER-011 — account-centric trigger path.
 * Fast-path classify_n is unchanged. This supplements it.
 * No RPC. Unknown CPI never produces S'.
 */

#define TRIG_DROP              0
#define TRIG_INCOMPLETE        1
#define TRIG_INVALID           2
#define TRIG_ALT_MISS          3
#define TRIG_EXACT             4
#define TRIG_RELEVANT_UNKNOWN  5
#define TRIG_ALT_UNCERTAIN     6

#define TRIG_LOADED_MAX        256u
#define TRIG_LUT_MAX           8u
#define TRIG_SCAN_MAX          8u

typedef struct {
    uint8_t  klass;
    uint8_t  versioned;
    uint8_t  nlut;
    uint8_t  alt_miss;
    uint8_t  alt_status;
    uint8_t  relevant;
    uint8_t  n_watch;
    uint16_t n_static;
    uint16_t n_loaded;
    uint32_t tx_off;
    uint32_t tx_end;
    uint8_t  outer[32];
    uint8_t  alt_miss_pk[32];
    uint8_t  sig[64];
    uint8_t  have_sig;
    uint32_t hit_pool[8];
    uint8_t  hit_proto[8];
    swapix_t exact;
} trigger_hit_t;

typedef struct {
    uint64_t scan;
    uint64_t framed;
    uint64_t alt_hit;
    uint64_t alt_miss;
    uint64_t alt_uncertain;
    uint64_t relevant;
    uint64_t exact;
    uint64_t unknown;
    uint64_t drop;
    uint64_t ns_frame;
    uint64_t ns_alt;
    uint64_t ns_watch;
    uint64_t ns_decode;
} trigger_metrics_t;

int trigger_eval(const uint8_t *p, uint32_t len, uint32_t start,
                 alt_cache_t *alts, const watch_idx_t *watch,
                 trigger_hit_t *out, trigger_metrics_t *m);
int trigger_eval_at(const uint8_t *p, uint32_t len, uint32_t start,
                    alt_cache_t *alts, const watch_idx_t *watch,
                    const alt_bank_t *bank, trigger_hit_t *out, trigger_metrics_t *m);
/* Shared admission after reassembly, including candidates from static decoders.
 * EXACT certifies resolved keys plus one recognized instruction, not all CPIs. */
int trigger_admit_at(const uint8_t *p, uint32_t len, const uint8_t signature[64],
                     alt_cache_t *alts, const watch_idx_t *watch,
                     const alt_bank_t *bank, trigger_hit_t *out);

/*
 * Scan a shred payload for generic txs. classify_n remains the fast path.
 * Writes the first EXACT, else first RELEVANT_UNKNOWN / ALT_MISS.
 * 0 if a hit was written, -1 if nothing relevant.
 */
int trigger_scan(const uint8_t *p, uint32_t len,
                 alt_cache_t *alts, const watch_idx_t *watch,
                 trigger_hit_t *out, trigger_metrics_t *m);
int trigger_scan_at(const uint8_t *p, uint32_t len,
                    alt_cache_t *alts, const watch_idx_t *watch,
                    const alt_bank_t *bank, trigger_hit_t *out, trigger_metrics_t *m);

#endif
