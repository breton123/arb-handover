#ifndef FIRM_SHRED_STREAM_H
#define FIRM_SHRED_STREAM_H

#include <stdint.h>

/*
 * Per-slot ledger byte stream. Caller stores shreds by index.
 * Append only at the contiguous watermark. Incremental Vec<Entry>
 * parser. DATA_COMPLETE does not reset framing.
 */

#define SHRED_STREAM_TX_MAX 64u
#define SHRED_STREAM_VEC_MAX 4096u
#define SHRED_STREAM_BUF (2u * 1024u * 1024u)

#define STREAM_ST_VEC 0u
#define STREAM_ST_ENT 1u
#define STREAM_ST_TX  2u

typedef struct {
    uint32_t n_ent;
    uint32_t n_tx;
    uint32_t n_legacy;
    uint32_t n_v0;
    uint32_t n_v1;
    uint32_t n_pump;
    uint32_t consumed;
    uint32_t tx_off[SHRED_STREAM_TX_MAX];
    uint32_t tx_len[SHRED_STREAM_TX_MAX];
} shred_stream_hit_t;

typedef struct {
    uint64_t auth_shreds;
    uint64_t contig_bytes;
    uint64_t slot_stream_bytes;
    uint64_t watermark_advances;
    uint64_t leftover_bytes;
    uint64_t entries;
    uint64_t entries_incremental;
    uint64_t txs;
    uint64_t tx_legacy;
    uint64_t tx_v0;
    uint64_t tx_v1;
    uint64_t pump;
    uint64_t sig_verify_ok;
    uint64_t sig_verify_fail;
    uint64_t frozen;
    uint64_t gap_blocked;
} shred_stream_stats_t;

typedef struct {
    uint8_t  live;
    uint8_t  frozen;
    uint8_t  pst;
    uint32_t wm;
    uint32_t buf_len;
    uint32_t parse_off;
    uint64_t vec_left;
    uint64_t tx_left;
    uint64_t slot;
    uint64_t tsc_origin;
    uint64_t tsc_first;
    uint64_t tsc_contig;
    uint64_t tsc_entry;
    uint64_t tsc_last;
    uint32_t entry_ord;
    uint32_t tx_in_ent;
    uint32_t n_shreds;
    uint32_t n_fec;
    uint32_t emit_n;
    uint32_t emit_len;
    uint32_t emit_off[SHRED_STREAM_TX_MAX];
    uint32_t emit_len_i[SHRED_STREAM_TX_MAX];
    uint32_t emit_ent[SHRED_STREAM_TX_MAX];
    uint32_t emit_txi[SHRED_STREAM_TX_MAX];
    uint8_t *buf;
    uint8_t *emit;
} shred_slot_stream_t;

int shred_slot_stream_init(shred_slot_stream_t *s);
void shred_slot_stream_reset(shred_slot_stream_t *s);
void shred_slot_stream_free(shred_slot_stream_t *s);

int shred_slot_stream_append(shred_slot_stream_t *s, const uint8_t *payload,
                             uint16_t plen, uint64_t tsc, int recovered,
                             shred_stream_stats_t *st);

void shred_slot_stream_parse(shred_slot_stream_t *s, uint64_t slot,
                             shred_stream_stats_t *st);

int shred_slot_stream_take(shred_slot_stream_t *s, uint8_t *dst,
                           uint32_t dst_cap, uint32_t *offs, uint32_t *lens,
                           uint32_t *ents, uint32_t *txis, uint32_t max_tx,
                           uint32_t *n_tx, uint32_t *blen, uint32_t *n_shreds,
                           uint32_t *n_fec);

uint32_t shred_slot_stream_leftover(const shred_slot_stream_t *s);

int shred_stream_parse(const uint8_t *buf, uint32_t len, uint64_t slot,
                       shred_stream_hit_t *out);

#endif
