#ifndef ARB_CORE_ALT_CACHE_H
#define ARB_CORE_ALT_CACHE_H

#include <stdint.h>

#define ALT_CACHE_CAP   1024u
#define ALT_ADDR_MAX    256u
#define ALT_PEND_MAX    256u
#define ALT_MAGIC       0x31544c41u /* "ALT1" */
#define ALT_MAGIC_V2    0x32544c41u /* "ALT2", little endian */
#define ALT_HAS_META    1u
#define ALT_FINALIZED   2u
#define ALT_HAS_HASH    4u
#define ALT_QUARANTINED 8u

typedef struct {
    uint64_t observed_slot;
    uint64_t last_extended_slot;
    uint64_t deactivation_slot;
    uint8_t  start_index;
    uint8_t  has_authority;
    uint8_t  reserved[6];
    uint8_t  observed_bank_hash[32];
    uint8_t  authority[32];
    uint8_t  account_hash[32];
    uint64_t received_unix_ns;
} alt_meta_t;

typedef struct { uint64_t slot; uint8_t hash[32]; } alt_slot_hash_t;
/* Caller supplies the actual target bank's coherent SlotHashes, not an RPC
 * snapshot from a different slot or an arbitrary ancestor list. */
typedef struct {
    uint64_t slot;
    uint8_t bank_hash[32];
    const alt_slot_hash_t *slot_hashes;
    uint16_t n_slot_hashes;
    uint8_t slot_hashes_complete;
} alt_bank_t;

enum {
    ALT_VALID = 0, ALT_MISSING = 1, ALT_META_UNKNOWN = 2,
    ALT_BANK_UNPROVEN = 3, ALT_FUTURE_OBSERVATION = 4,
    ALT_DEACTIVATED = 5, ALT_CONFLICT = 6, ALT_INDEX_UNAVAILABLE = 7
};

typedef struct {
    uint8_t  used;
    uint8_t  pk[32];
    uint16_t n;
    uint16_t flags;
    alt_meta_t meta;
    uint8_t  addr[ALT_ADDR_MAX * 32u];
} alt_ent_t;

typedef struct {
    alt_ent_t *ent;
    uint32_t   cap;
    uint32_t   n;
    uint64_t   hit;
    uint64_t   miss;
    uint64_t   put;
    uint8_t    pend[ALT_PEND_MAX][32];
    uint32_t   n_pend;
    uint64_t   unique;
} alt_cache_t;

int alt_cache_init(alt_cache_t *c);
void alt_cache_free(alt_cache_t *c);

/* Control plane only. Hot path is get. */
int alt_cache_put(alt_cache_t *c, const uint8_t pk[32],
                  const uint8_t *addrs, uint16_t n);
int alt_cache_put_meta(alt_cache_t *c, const uint8_t pk[32],
                      const uint8_t *addrs, uint16_t n,
                      const alt_meta_t *meta, uint16_t flags);

/* Address facts only: a hit is NOT a lifecycle/fork validity certificate. */
int alt_cache_get(const alt_cache_t *c, const uint8_t pk[32],
                  const uint8_t **addrs, uint16_t *n);
int alt_cache_check(const alt_cache_t *c, const uint8_t pk[32],
                    const alt_bank_t *bank, uint16_t *active_n);

/* Address + deactivation/extension facts only. Not a SlotHashes certificate.
 * Used on the live shred path when the target bank identity is unproven. */
int alt_cache_live_active_n(const alt_cache_t *c, const uint8_t pk[32],
                            uint64_t tx_slot, uint16_t *active_n);

/* Enqueue an unresolved ALT for the off-path fetcher. 0 queued. */
int alt_cache_note_miss(alt_cache_t *c, const uint8_t pk[32]);

/* Persist / reload resolved tables. Hot path never fetches. */
int alt_cache_save(const alt_cache_t *c, const char *path);
int alt_cache_load(alt_cache_t *c, const char *path);

#endif
