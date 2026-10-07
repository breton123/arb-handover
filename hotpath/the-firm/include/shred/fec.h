#ifndef FIRM_SHRED_FEC_H
#define FIRM_SHRED_FEC_H

#include "shred/shred.h"

#include <stdint.h>

#define SHRED_FEC_OUT_MAX 67u

typedef struct {
    uint32_t possible;
    uint32_t attempted;
    uint32_t recovered;
    uint32_t auth_ok;
    uint32_t auth_fail;
    uint64_t recovery_cpu_ns;
    uint64_t auth_cpu_ns;
    uint64_t parse_cpu_ns;
} shred_fec_stats_t;

typedef struct {
    uint64_t slot;
    uint32_t index;
    uint32_t fec;
    uint16_t len;
    uint64_t tsc_first;
    uint64_t tsc_enough;
    uint8_t  pkt[SHRED_MAX_SZ];
} shred_fec_rec_t;

typedef struct shred_fec shred_fec_t;

int shred_fec_init(shred_fec_t **out);
void shred_fec_free(shred_fec_t *f);
const shred_fec_stats_t *shred_fec_stats(const shred_fec_t *f);

/*
 * Ingest one shred. On successful recover, writes reconstructed data
 * shreds to rec[0..*nrec). 0 = no new recover. 1 = recovered. -1 = ignore.
 */
int shred_fec_push(shred_fec_t *f, const uint8_t *pkt, uint16_t len,
                   uint64_t rx_tsc, shred_fec_rec_t *rec, uint32_t cap,
                   uint32_t *nrec);

/* Merkle root implied by one shred's proof. Diagnostic / partition key. */
int shred_fec_merkle_root(const uint8_t *pkt, uint16_t len, uint8_t root[32]);

#endif
