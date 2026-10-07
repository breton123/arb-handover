#ifndef FIRM_SHRED_ENTRY_H
#define FIRM_SHRED_ENTRY_H

#include "shred/txframe.h"

#include <stdint.h>

/*
 * Vec<Entry> over a contiguous authenticated prefix. One bad
 * transaction does not freeze the slot: it stops that entry vector
 * with STREAM_DESYNC so later code can still see prior frames.
 */

#define ENT_OK            0
#define ENT_NEED_MORE     1
#define ENT_BAD_HEADER    2
#define ENT_STREAM_DESYNC 3

#define ENT_VEC_MAX  4096u
#define ENT_TX_MAX   4096u

typedef struct {
    uint32_t entry_ord;
    uint32_t tx_ord;
    uint32_t entry_off;
    uint32_t entry_len;
    uint32_t entry_tx_count;
    uint32_t consumed;
    uint32_t n_tx;
    const uint8_t *bytes;
    txframe_t tx;
} shred_entry_hit_t;

typedef void (*shred_entry_cb)(void *user, const shred_entry_hit_t *hit);

int shred_entries_parse(const uint8_t *buf, uint32_t len,
                        shred_entry_cb cb, void *user, uint32_t *consumed,
                        int *rc_out);

#endif
