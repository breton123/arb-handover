#ifndef FIRM_ORACLE_SOAK_H
#define FIRM_ORACLE_SOAK_H

#include <stdint.h>

/*
 * PUMP-ORACLE-002 corpus-scale admission. Jsonl produced by
 * tools/oracle/import_soak.py from state008/mismatch (+ n015_tx).
 */

typedef struct {
    uint32_t usable;
    uint32_t exact;
    uint32_t fallback;
    uint32_t unknown;
    uint32_t unexplained;
    uint32_t sell;
    uint32_t buy_eq;
    uint32_t buy_out;
    uint32_t multi_cpi;
    uint32_t wsol;
    uint32_t assoc;
    uint32_t missing_v;
    uint32_t missing_fee;
    uint32_t recovered_v;
    uint32_t wrong_n;
    uint32_t token2022;
    uint32_t unsupported;
    uint32_t exact_wrong;
    uint32_t skipped;
} oracle_soak_stats_t;

void oracle_soak_stats_clear(oracle_soak_stats_t *st);

/* Stream-admit a jsonl corpus. Returns 0 even if unexplained > 0. */
int oracle_admit_jsonl(const char *path, oracle_soak_stats_t *st);

void oracle_soak_print(const oracle_soak_stats_t *st);

#endif /* FIRM_ORACLE_SOAK_H */
