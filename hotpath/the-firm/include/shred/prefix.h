#ifndef FIRM_SHRED_PREFIX_H
#define FIRM_SHRED_PREFIX_H

#include <stdint.h>

/*
 * Contiguous authenticated data-shred prefix from index 0.
 * A later complete island does not fill an earlier hole.
 * No entry/tx parse here.
 */

#define PREFIX_SLOT_MAX 32u
#define PREFIX_ROW_MAX  2048u
#define PREFIX_BATCH_MAX 256u
#define PREFIX_BUF      (2u * 1024u * 1024u)

/* Agave data shred: DATA_COMPLETE=0x40, last-in-slot=0xC0.
 * Do not use the old complete.h 0x80 specimen. */
#define PREFIX_FLAG_DATA_COMPLETE 0x40u

typedef struct {
    uint64_t slot;
    uint32_t first_hole;
    uint32_t n_data;
    uint32_t n_batch;
    uint32_t concat_len;
    uint8_t  have0;
} shred_prefix_info_t;

typedef struct shred_prefix shred_prefix_t;

int shred_prefix_init(shred_prefix_t **out);
void shred_prefix_free(shred_prefix_t *p);
int shred_prefix_watch(shred_prefix_t *p, uint64_t slot);
int shred_prefix_push(shred_prefix_t *p, const uint8_t *pkt, uint16_t len);
int shred_prefix_info(const shred_prefix_t *p, uint64_t slot,
                      shred_prefix_info_t *out);
int shred_prefix_bytes(const shred_prefix_t *p, uint64_t slot,
                       const uint8_t **buf, uint32_t *len);

/* Exclusive byte ends of each DATA_COMPLETE batch. Trailing shreds
 * after the last complete marker are not a batch. */
int shred_prefix_batches(const shred_prefix_t *p, uint64_t slot,
                         uint32_t *end_off, uint32_t cap, uint32_t *n);

#endif
