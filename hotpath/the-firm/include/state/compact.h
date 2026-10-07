#ifndef FIRM_STATE_COMPACT_H
#define FIRM_STATE_COMPACT_H

#include "ingress/tx.h"
#include "protocols/pump/pump.h"
#include "state/cert.h"

#include <stdint.h>

#define STATE_POOL_MAX   1024u
#define STATE_TOKEN_MAX  2048u
#define STATE_SYS_MAX    128u

/* Compact ids are 0..n-1. STATE_ACCT_NONE must never alias a live row. */
_Static_assert(STATE_ACCT_NONE != 0u,
               "id 0 is a valid CompactState row; do not use 0 as unset");

#define STATE_PROTO_PUMP  PROTO_PUMP
#define STATE_PROTO_DLMM  PROTO_DLMM

#define TOKEN_PROG_NONE   0u
#define TOKEN_PROG_SPL    1u
#define TOKEN_PROG_2022   2u

#define TOKEN_FLAG_WSOL   1u

#define POOL_AUTH_VQ     1u
#define POOL_AUTH_FEE    2u
#define POOL_AUTH_VAULT  4u
#define POOL_AUTH_MINT   8u
#define POOL_AUTH_READY  15u

#define POOL_GRADE_UNKNOWN 0u
#define POOL_GRADE_EXACT   1u
#define POOL_GRADE_DIRTY   2u

typedef struct {
    uint64_t slot;
    uint32_t entry_index;
    uint32_t tx_index;
    uint64_t version;
} state_bank_t;

typedef struct {
    uint8_t      pubkey[32];
    uint8_t      proto;
    uint8_t      live;
    uint8_t      auth_bits; /* POOL_AUTH_*; exact publish needs READY */
    uint8_t      grade;     /* POOL_GRADE_* */
    uint64_t     incarnation; /* generation after each exact publish */
    uint64_t     anchor_slot; /* finalized getMultipleAccounts context.slot */
    uint8_t      vault_base[32];
    uint8_t      vault_quote[32];
    pump_state_t pump;
} state_pool_t;

typedef struct {
    uint8_t  pubkey[32];
    uint8_t  mint[32];
    uint8_t  owner[32];
    uint64_t amount;
    uint64_t native_reserve;
    uint64_t incarnation;
    uint8_t  token_program;
    uint8_t  flags;
    uint16_t extensions_mask; /* nonzero Token-2022 → fail closed */
    uint8_t  live;
} state_token_t;

typedef struct {
    uint8_t  pubkey[32];
    uint64_t lamports;
    uint64_t incarnation;
    uint8_t  live;
} state_sys_t;

typedef struct {
    state_bank_t bank;
    state_cert_t cert;
    uint32_t     n_pool;
    uint32_t     n_token;
    uint32_t     n_sys;
    state_pool_t pool[STATE_POOL_MAX];
    state_token_t token[STATE_TOKEN_MAX];
    state_sys_t  sys[STATE_SYS_MAX];
} compact_state_t;

void compact_state_clear(compact_state_t *st);

int compact_pool_add_pump(compact_state_t *st, const uint8_t pubkey[32],
                          const pump_state_t *s, uint32_t *id_out);

/*
 * AUTHSTATE-001. Arm compact Pump fields + vault identities.
 * Re-arm overwrites DIRTY rows. Sets this row's anchor_slot to the
 * account-fetch context.slot (finalized bank, not GetSlot, not processed).
 */
int compact_pool_arm(compact_state_t *st, const uint8_t pool[32],
                     const uint8_t vault_base[32], const uint8_t vault_quote[32],
                     const pump_state_t *s, uint64_t anchor_slot,
                     uint32_t *id_out);

int compact_pool_get(const compact_state_t *st, uint32_t id,
                     const state_pool_t **out);

int compact_token_add(compact_state_t *st, const state_token_t *t,
                      uint32_t *id_out);

/* Reserve a dead token row for later ATA create/init. */
int compact_token_reserve(compact_state_t *st, const uint8_t pubkey[32],
                          uint32_t *id_out);

int compact_token_get(const compact_state_t *st, uint32_t id,
                      const state_token_t **out);

int compact_sys_add(compact_state_t *st, const state_sys_t *s,
                    uint32_t *id_out);

int compact_sys_get(const compact_state_t *st, uint32_t id,
                    const state_sys_t **out);

int compact_pool_find(const compact_state_t *st, const uint8_t pk[32],
                      uint32_t *id_out);
int compact_pool_mark_dirty(compact_state_t *st, uint32_t id);
int compact_token_find(const compact_state_t *st, const uint8_t pk[32],
                       uint32_t *id_out);
int compact_sys_find(const compact_state_t *st, const uint8_t pk[32],
                     uint32_t *id_out);

#endif /* FIRM_STATE_COMPACT_H */
