#include "oracle/case.h"

#include <stdio.h>
#include <string.h>

static const char *
cls(uint8_t c)
{
    switch (c) {
    case ORACLE_FAST_CUSTOM_EXACT:
        return "FAST_CUSTOM_EXACT";
    case ORACLE_FALLBACK_REQUIRED:
        return "FALLBACK_REQUIRED";
    case ORACLE_UNKNOWN:
        return "UNKNOWN";
    case ORACLE_UNEXPLAINED:
        return "UNEXPLAINED";
    default:
        return "OTHER";
    }
}

int
main(int argc, char **argv)
{
    oracle_case_t cases[ORACLE_CASE_MAX];
    uint32_t i;
    int n;
    uint32_t n_exact = 0, n_fb = 0, n_unk = 0, n_unex = 0, n_mismatch = 0;
    const char *path = NULL;

    memset(cases, 0, sizeof(cases));
    if (argc >= 3 && strcmp(argv[1], "--corpus") == 0) {
        path = argv[2];
    }
    n = oracle_load_corpus(path, cases, ORACLE_CASE_MAX);
    if (n <= 0) {
        fprintf(stderr, "PUMP-ORACLE-001 corpus not found or empty\n");
        return 1;
    }
    for (i = 0; i < (uint32_t)n; i++) {
        oracle_case_result_t r;

        if (oracle_run_case(&cases[i], &r) != 0) {
            fprintf(stderr, "FAIL  run %s\n", cases[i].id);
            n_unex++;
            oracle_case_clear(&cases[i]);
            continue;
        }
        if (r.judged == ORACLE_FAST_CUSTOM_EXACT) {
            n_exact++;
        } else if (r.judged == ORACLE_FALLBACK_REQUIRED) {
            n_fb++;
        } else if (r.judged == ORACLE_UNKNOWN) {
            n_unk++;
        } else {
            n_unex++;
        }
        if (r.judged != r.expect) {
            fprintf(stderr, "FAIL  %s judged %s expect %s apply=%d dep=%u "
                            "pool=%u tok=%u sys=%u inc=%u cert=%u ov=%u\n",
                    r.id, cls(r.judged), cls(r.expect), r.apply_rc,
                    r.dep_class, r.diff.n_pool_mismatch,
                    r.diff.n_token_mismatch, r.diff.n_sys_mismatch,
                    r.diff.n_incarnation_mismatch, r.diff.n_cert_mismatch,
                    r.diff.n_overlay_mismatch);
            n_mismatch++;
        } else {
            printf("ok    %s %s\n", r.id, cls(r.judged));
        }
        oracle_case_clear(&cases[i]);
    }

    printf("\nPUMP-ORACLE-001  cases=%d  FAST_CUSTOM_EXACT=%u  "
           "FALLBACK_REQUIRED=%u  UNKNOWN=%u  UNEXPLAINED=%u\n",
           n, n_exact, n_fb, n_unk, n_unex);
    if (n_unex != 0 || n_mismatch != 0) {
        fprintf(stderr, "unexplained or label mismatch — DLMM is blocked\n");
        return 1;
    }
    printf("held-out: 0 unexplained mismatches\n");
    return 0;
}
