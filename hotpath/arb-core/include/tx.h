#ifndef ARB_CORE_TX_H
#define ARB_CORE_TX_H

#include <stdint.h>

#define TX_SIG_MAX       12u
#define TX_KEY_MAX       64u
#define TX_INSTR_MAX     64u
#define TX_KEY_SZ        32u
#define TX_SIG_SZ        64u
#define TX_BLOCKHASH_SZ  32u

#define PROTO_NONE  0u
#define PROTO_DLMM  1u
#define PROTO_PUMP  2u

typedef struct {
    const uint8_t *keys;
    uint16_t nkeys;
    uint16_t keys_off;
    uint16_t msg_off;
    uint8_t  nsig;
    uint8_t  versioned;
} msg_keys_t;

typedef struct {
    uint8_t protocol;
    const uint8_t *key;
} pool_cand_t;

int cu16_dec(const uint8_t *p, uint16_t avail, uint16_t *val, uint16_t *used);

/* Recover the static account-key array that contains a program id at prog_off. */
int msg_keys_from_prog(const uint8_t *p, uint16_t len, uint16_t prog_off,
                       msg_keys_t *out);

/*
 * Walk instructions after a recovered key list.
 * On success with a DLMM/Pump ix, writes pool candidate (ix account[0]).
 * Returns 0 if a dex pool candidate was named, -1 otherwise.
 */
int msg_first_dex_pool(const uint8_t *p, uint16_t len, const msg_keys_t *mk,
                       pool_cand_t *out);

/* Find first DLMM/Pump 32-byte program id. Returns 0 on hit. */
int find_prog_id(const uint8_t *p, uint16_t len, uint16_t *off, uint8_t *proto);

/* Next program id at or after `from`. */
int find_next_prog_id(const uint8_t *p, uint16_t len, uint16_t from,
                      uint16_t *off, uint8_t *proto);

/*
 * Parse a legacy/V0 transaction starting at `start`.
 * Does not require the txn to consume the rest of the buffer.
 * Returns 0 if a DLMM/Pump pool candidate was named.
 */
int txn_dex_at(const uint8_t *p, uint16_t len, uint16_t start,
               msg_keys_t *mk, pool_cand_t *cand);

#endif /* ARB_CORE_TX_H */
