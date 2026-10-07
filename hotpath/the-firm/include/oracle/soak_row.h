#ifndef FIRM_ORACLE_SOAK_ROW_H
#define FIRM_ORACLE_SOAK_ROW_H

#include <stdint.h>

#define SOAK_ID_MAX   80u
#define SOAK_TAG_MAX  32u
#define SOAK_SIG_MAX  160u

#define SOAK_IX_SELL     1u
#define SOAK_IX_BUY_EQ   2u
#define SOAK_IX_BUY_OUT  3u
#define SOAK_IX_DLMM     4u
#define SOAK_IX_OTHER    5u

typedef struct {
    char     id[SOAK_ID_MAX];
    char     tag[SOAK_TAG_MAX];
    char     sighex[SOAK_SIG_MAX];
    uint64_t slot;
    uint64_t ain;
    uint64_t rb, rq, prb, prq;
    int64_t  vq;
    uint64_t lp, proto, cr;
    uint32_t n_cpi;
    uint8_t  dir;
    uint8_t  ix;
    uint8_t  pre_complete;
    uint8_t  wsol;
    uint8_t  t22;
} soak_row_t;

#endif /* FIRM_ORACLE_SOAK_ROW_H */
