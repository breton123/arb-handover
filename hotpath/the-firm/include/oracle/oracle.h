#ifndef FIRM_ORACLE_ORACLE_H
#define FIRM_ORACLE_ORACLE_H

#include "deps/class.h"
#include "ingress/tx.h"
#include "protocols/pump/pump.h"
#include "state/compact.h"
#include "transitions/apply.h"
#include "transitions/overlay.h"

#include <stdint.h>

/*
 * PUMP-ORACLE-001. Offline only. No RPC.
 *
 * Input: certified prestate + exact OrderedTx + reference-bank poststate.
 * Output: field diff + admission class.
 */

#define ORACLE_EXACT       0
#define ORACLE_MISMATCH    1
#define ORACLE_REJECTED    2

#define ORACLE_FAST_CUSTOM_EXACT  10u
#define ORACLE_FALLBACK_REQUIRED  11u
#define ORACLE_UNKNOWN            12u
#define ORACLE_UNEXPLAINED        13u

typedef struct {
    uint64_t reserve_base;
    uint64_t reserve_quote;
    int64_t  virtual_quote;
    uint64_t slot;
} oracle_pump_t;

typedef struct {
    uint32_t n_pool_mismatch;
    uint32_t n_token_mismatch;
    uint32_t n_sys_mismatch;
    uint32_t n_incarnation_mismatch;
    uint32_t n_cert_mismatch;
    uint32_t n_overlay_mismatch;
    uint8_t  class_ours;
    uint8_t  class_expect;
    uint8_t  judged;
} oracle_diff_t;

int oracle_pump_eq(const pump_state_t *ours, const oracle_pump_t *auth);

int oracle_diff_state(const compact_state_t *ours, const compact_state_t *auth,
                      oracle_diff_t *out);

int oracle_diff_overlay(const tx_overlay_t *ov, const compact_state_t *auth,
                        oracle_diff_t *out);

int oracle_diff_cert(const state_cert_t *ours, const ordered_tx_t *tx,
                     uint8_t expect_exactness, oracle_diff_t *out);

/*
 * Admission rule (harsh):
 *   complete pre + apply OK + all compared fields match → FAST_CUSTOM_EXACT
 *   apply REJECT + UNKNOWN → UNKNOWN
 *   apply REJECT + FALLBACK, or incomplete pre → FALLBACK_REQUIRED
 *   complete pre + apply OK + any mismatch → UNEXPLAINED
 *   complete pre + unexpected reject/abort → UNEXPLAINED
 */
uint8_t oracle_judge(int apply_rc, uint8_t dep_class, uint8_t pre_complete,
                     int fields_exact);

int oracle_replay(const compact_state_t *pre, const ordered_tx_t *tx,
                  const compact_state_t *certified_post, uint8_t expect_class,
                  oracle_diff_t *out);

#endif /* FIRM_ORACLE_ORACLE_H */
