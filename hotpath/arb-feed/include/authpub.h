#ifndef ARB_FEED_AUTHPUB_H
#define ARB_FEED_AUTHPUB_H

#include <stddef.h>
#include <stdint.h>

/*
 * STATE-009 AUTH-PUBLISH — lock-free per-pool coherent ring.
 * STATE-008 writes. PAPER reads. No JSON. No RPC. No full-univ copy.
 *
 * Layout must match arb-state/shyft/authpub.py.
 */

#define AUTH_MAGIC      0x39304841u /* AH09 */
#define AUTH_VER        1u
#define AUTH_RING       16u
#define AUTH_BLOB       2048u
#define AUTH_HDR        256u
#define AUTH_ENT        2176u
#define AUTH_POOL_HDR   64u
#define AUTH_POOL       (AUTH_POOL_HDR + (AUTH_RING * AUTH_ENT))
#define AUTH_POOL_CAP   4096u

#define AUTH_COHERENT   1u
#define AUTH_INCOMPLETE 2u
#define AUTH_GAPPED     4u
#define AUTH_BOOT       8u

#define AUTH_PATH_DEFAULT "/dev/shm/arb_auth009"

typedef struct {
    uint64_t generation;
    uint64_t slot;
    uint64_t state_version;
    uint64_t hash;
    uint32_t txn_index;
    uint32_t flags;
    uint8_t  sig[64];
    uint8_t  proto;
    uint8_t  coherent;
    uint8_t  ok;
} authpub_meta_t;

int authpub_open(const char *path);
void authpub_close(void);
int authpub_ready(void);
uint64_t authpub_hash(const void *p, size_t n);

/* Predecessor of trigger N: latest coherent state strictly before N.
 * If the ring already contains N's signature, returns the entry before it.
 * Same-slot entries without a sig match are skipped (may be after N). */
int authpub_pred(uint32_t pool_idx, uint64_t trig_slot, const uint8_t sig[64],
                 authpub_meta_t *meta, uint8_t *blob, uint16_t *blob_len);

#endif
