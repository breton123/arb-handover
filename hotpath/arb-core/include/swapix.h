#ifndef ARB_CORE_SWAPIX_H
#define ARB_CORE_SWAPIX_H

#include "hot.h"
#include "tx.h"

#include <stdint.h>

/*
 * Live-path swap extract. Beside frozen hot_decode_trigger.
 *
 * Supported variants (anything else → fail closed):
 *   DLMM swap2 / swap     amount_in, min_out; swap_for_y from ATA
 *   Pump sell             amount_in = base_in
 *   Pump buy_exact_quote_in  amount_in = spendable_quote
 *   Pump buy (exact-out)  unsupported
 *
 * Signature required. No sig → no decide. That is what stops
 * one transaction across shred fragments from becoming N decisions.
 */

#define SWAPIX_SEEN_CAP  (1u << 18)

typedef struct {
    uint8_t  protocol;
    uint8_t  pool[32];
    uint8_t  sig[64];
    uint64_t amount_in;
    uint64_t min_out;
    uint8_t  direction; /* DLMM: swap_for_y. Pump: PUMP_DIR_* */
    uint8_t  have_sig;
    uint8_t  variant;   /* 1 swap2 2 swap1 3 sell 4 buy_eq */
} swapix_t;

typedef struct {
    uint8_t *sig;   /* cap * 64 */
    uint8_t *used;  /* cap */
    uint32_t cap;
    uint32_t n;
} hot_seen_t;

int swapix_from_msg(const uint8_t *p, uint16_t len, const msg_keys_t *mk,
                    swapix_t *out);

int swapix_from_tx(const uint8_t *p, uint16_t len, uint16_t start,
                   swapix_t *out);

/* Scan payload (shred body or raw tx). Requires recoverable signature. */
int swapix_from_payload(const uint8_t *p, uint16_t len, swapix_t *out);

/*
 * Walk instructions using the LOADED key vector (static + ALT).
 * n_static is the on-wire static key count. n_loaded is the resolved count.
 * Program id may live in the ALT. Does not invent amounts.
 */
int swapix_from_loaded(const uint8_t *p, uint16_t len,
                       uint16_t keys_off, uint16_t n_static,
                       const uint8_t *loaded, uint16_t n_loaded,
                       swapix_t *out);

int swapix_to_trigger(const swapix_t *ix, hot_trigger_t *out);

/* Geyser / replay: resolved key vector + one ix. Production decode_ix. */
int swapix_from_flat(const uint8_t *keys, uint16_t nkeys,
                     const uint8_t *acc, uint16_t nacct,
                     const uint8_t *data, uint16_t dlen,
                     swapix_t *out);

int hot_seen_init(hot_seen_t *s);
void hot_seen_free(hot_seen_t *s);

/* 1 = first time (caller should decide). 0 = already seen. -1 = error. */
int hot_seen_first(hot_seen_t *s, const uint8_t sig[64]);

#endif /* ARB_CORE_SWAPIX_H */
