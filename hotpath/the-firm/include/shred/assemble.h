#ifndef FIRM_SHRED_ASSEMBLE_H
#define FIRM_SHRED_ASSEMBLE_H

#include "shred/fec.h"
#include "shred/stream.h"

#include <stdint.h>

/*
 * Slot-local data-shred assembly. Inserts by index, advances a
 * contiguous watermark, parses Vec<Entry> incrementally. FEC recover
 * fills gaps. DATA_COMPLETE does not reset the parser.
 */

#define RECON_NONE   0u
#define RECON_ONE    1u
#define RECON_MULTI  2u
#define RECON_FEC    3u

#define ASSEM_TX_MAX  SHRED_STREAM_TX_MAX
#define ASSEM_BUF_MAX (256u * 1024u)

typedef struct {
    uint8_t  how;
    uint8_t  n_tx;
    uint8_t  fec_possible;
    uint64_t slot;
    uint64_t tsc_first;
    uint64_t tsc_enough;
    uint64_t tsc_contig;
    uint64_t tsc_entry;
    uint64_t tsc_done;
    uint32_t n_shreds;
    uint32_t tx_off[ASSEM_TX_MAX];
    uint32_t tx_len[ASSEM_TX_MAX];
    uint32_t tx_entry[ASSEM_TX_MAX];
    uint32_t tx_ord[ASSEM_TX_MAX];
    uint32_t blen;
    uint8_t  buf[ASSEM_BUF_MAX];
} shred_batch_t;

typedef struct shred_assem shred_assem_t;

int shred_assem_init(shred_assem_t **out);
void shred_assem_free(shred_assem_t *a);

uint32_t shred_assem_incomplete(const shred_assem_t *a);
uint32_t shred_assem_fec_possible(const shred_assem_t *a);
const shred_fec_stats_t *shred_assem_fec_stats(const shred_assem_t *a);
const shred_stream_stats_t *shred_assem_stream_stats(const shred_assem_t *a);

int shred_assem_push(shred_assem_t *a, const uint8_t *pkt, uint16_t len,
                     uint64_t rx_tsc, shred_batch_t *batch);
int shred_assem_next(shred_assem_t *a, shred_batch_t *batch);

#endif
