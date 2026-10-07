#ifndef FIRM_STATE_AUTHVEC_H
#define FIRM_STATE_AUTHVEC_H

#include "ingress/tx.h"
#include "protocols/pump/pump.h"
#include "state/compact.h"

#include <stdint.h>

/*
 * AUTHSTATE-001. Stage later AUTH writes by (slot, signature, account).
 * Compare a prediction only when the required Pump dependency set for
 * that committed transaction has arrived. Never fill holes from another
 * signature or from mutable "latest" state.
 */

#define AUTH_PRED_MAX   8192u
#define AUTH_WRITE_MAX  16384u

#define AUTH_PEND       0u
#define AUTH_EXACT      1u
#define AUTH_MISMATCH   2u
#define AUTH_INCOMPLETE 3u

#define AUTH_ROLE_POOL  1u
#define AUTH_ROLE_VB    2u
#define AUTH_ROLE_VQ    3u

typedef struct {
    uint8_t      sig[STATE_SIG_LEN];
    uint8_t      pubkey[32];
    uint64_t     slot;
    uint64_t     write_version;
    uint8_t      role;
    uint8_t      have_vq;
    uint8_t      have_fees;
    uint8_t      have_reserves;
    uint8_t      have_amount;
    uint8_t      have_disabled;
    uint8_t      have_status;
    uint64_t     amount;
    pump_state_t pump;
} auth_write_t;

typedef struct {
    uint8_t      sig[STATE_SIG_LEN];
    uint64_t     slot;
    uint32_t     pool_id;
    uint8_t      pool[32];
    uint8_t      vault_base[32];
    uint8_t      vault_quote[32];
    uint8_t      need_vb;
    uint8_t      need_vq;
    pump_state_t predicted;
    pump_state_t prestate;
    uint8_t      prestate_hash[32];
    uint64_t     incarnation;
    uint64_t     of_rx_tsc;
    uint64_t     state_ready_tsc;
    uint8_t      judged;
} auth_pred_t;

typedef struct {
    uint32_t    n_pred;
    uint32_t    n_write;
    uint64_t    n_exact;
    uint64_t    n_mismatch;
    uint64_t    n_incomplete;
    uint64_t    n_mixed;
    auth_pred_t pred[AUTH_PRED_MAX];
    auth_write_t write[AUTH_WRITE_MAX];
} authvec_t;

void authvec_clear(authvec_t *av);

int authvec_note_pred(authvec_t *av, const compact_state_t *st,
                      const ordered_tx_t *tx, uint32_t pool_id,
                      const pump_state_t *predicted);

int authvec_note_pred_full(authvec_t *av, const compact_state_t *st,
                           const ordered_tx_t *tx, uint32_t pool_id,
                           const pump_state_t *prestate,
                           const pump_state_t *predicted, uint64_t of_rx_tsc,
                           uint64_t state_ready_tsc);

int authvec_put_write(authvec_t *av, const auth_write_t *w);

int authvec_load_jsonl(authvec_t *av, const char *path);
int authvec_load_jsonl_from(authvec_t *av, const char *path, uint64_t *off);

void authvec_judge(authvec_t *av);

#endif
