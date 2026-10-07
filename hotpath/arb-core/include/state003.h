#ifndef ARB_CORE_STATE003_H
#define ARB_CORE_STATE003_H

#include "hot.h"
#include "live.h"

#include <stdio.h>
#include <stdint.h>

/*
 * STATE-003 — seeing N early is for deciding; authoritative execution is for S.
 * Frozen kernels stay frozen. This is policy beside hot_decide / hot_commit.
 *
 *   bootstrap → real RPC LbPair+bins            → SYNCED, sendable
 *   observe   → speculative S' → opportunity_t  NEVER writes canonical S
 *   confirm   → hot_commit after land, then SPECULATIVE (not sendable)
 *   reject    → failed / gone                   canonical unchanged, still sendable
 *   refresh   → authoritative LbPair+BinArrays  → SYNCED, sendable
 *
 * An authoritative RPC snapshot is authoritative as of its slot.
 * It stays sendable until a confirmed on-chain mutation (then SPECULATIVE
 * until refresh). No wall-clock TTL. auth_ns is bootstrap-or-refresh time.
 * SPECULATIVE after confirm means local apply awaiting account reconcile.
 * Only SYNCED is sendable.
 */

#define ST3_HEALTH_MISSING      0u
#define ST3_HEALTH_STALE        1u
#define ST3_HEALTH_SPECULATIVE  2u
#define ST3_HEALTH_SYNCED       3u

#define ST3_OK           0
#define ST3_NO_OPP       1
#define ST3_MISSING_BIN  2
#define ST3_UNHEALTHY    3
#define ST3_APPLY_FAIL   4

#define ST3_Q_CAP       4096u
#define ST3_RESYNC_CAP  4096u

typedef struct {
    uint32_t pool_idx;
    int32_t  bin_id;
    uint8_t  reason; /* 2 = missing bin */
} st3_need_t;

typedef struct {
    uint8_t   *health;
    uint64_t  *auth_slot;
    uint64_t  *auth_ns;
    uint64_t  *spec_since_ns;
    uint32_t   cap;
    st3_need_t q[ST3_Q_CAP];
    uint32_t   q_n;
    uint64_t   n_observe;
    uint64_t   n_confirm;
    uint64_t   n_reject;
    uint64_t   n_refresh;
    uint64_t   n_refresh_req;
    uint64_t   n_enqueue;
    uint64_t   n_trig_speculative;
    uint64_t   n_opp_suppressed;
    uint64_t   n_exact;
    uint64_t   n_mismatch;
    uint32_t   resync_ns[ST3_RESYNC_CAP];
    uint32_t   n_resync;
} state_mgr_t;

void state_mgr_init(state_mgr_t *m, const live_univ_t *u);
void state_mgr_free(state_mgr_t *m);

int state_sendable(const state_mgr_t *m, const live_univ_t *u, uint32_t pool_idx);

/* Speculative only. Canonical S and state_version unchanged. */
int state_observe(state_mgr_t *m, const live_univ_t *u, uint32_t pool_idx,
                  const hot_trigger_t *n, hot_decision_t *dec);

/* Authoritative success only. */
int state_confirm(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx,
                  const hot_trigger_t *n);

/* Failed or disappeared N. Canonical unchanged. */
int state_reject(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx);

int state_refresh(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx,
                  const dlmm_state_t *auth, uint64_t slot);

int state_refresh_pump(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx,
                       const pump_state_t *auth, uint64_t slot);

/* Local S' vs authoritative snap. 1 = economically equal. */
int state_recon_eq_dlmm(const live_univ_t *u, uint32_t pool_idx,
                        const dlmm_state_t *auth);
int state_recon_eq_pump(const live_univ_t *u, uint32_t pool_idx,
                        const pump_state_t *auth);

void state_mark_stale(state_mgr_t *m, uint32_t pool_idx);
void state_enqueue(state_mgr_t *m, uint32_t pool_idx, int32_t bin_id, uint8_t reason);

uint32_t state_resync_pct(const state_mgr_t *m, double p);
void state_metrics_print(const state_mgr_t *m, FILE *f);

#endif /* ARB_CORE_STATE003_H */
