#ifndef FIRM_ORACLE_CASE_H
#define FIRM_ORACLE_CASE_H

#include "ingress/tx.h"
#include "oracle/oracle.h"
#include "state/compact.h"

#include <stdint.h>

#define ORACLE_CASE_ID_MAX  64u
#define ORACLE_CASE_MAX     64u

typedef struct {
    char             id[ORACLE_CASE_ID_MAX];
    uint8_t          source;      /* 0 geyser 1 sdk 2 overlay016 3 spec */
    uint8_t          pre_complete;
    uint8_t          expect;      /* ORACLE_FAST_CUSTOM_EXACT / … */
    compact_state_t *pre;
    compact_state_t *post;
    ordered_tx_t     tx;
} oracle_case_t;

typedef struct {
    char          id[ORACLE_CASE_ID_MAX];
    uint8_t       expect;
    uint8_t       judged;
    int           apply_rc;
    uint8_t       dep_class;
    oracle_diff_t diff;
} oracle_case_result_t;

void oracle_case_clear(oracle_case_t *c);

int oracle_run_case(const oracle_case_t *c, oracle_case_result_t *out);

/*
 * Load PUMPOR1 text corpus. Caller frees each case with oracle_case_clear.
 * Returns number of cases or -1.
 */
int oracle_load_corpus(const char *path, oracle_case_t *out, uint32_t cap);

#endif /* FIRM_ORACLE_CASE_H */
