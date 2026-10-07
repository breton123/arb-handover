#ifndef ARB_EXEC_ROUTE0_H
#define ARB_EXEC_ROUTE0_H

#include "opportunity.h"

#include <stddef.h>
#include <stdint.h>

/*
 * EXEC-001 — route 0 instruction accounts.
 *
 * Order is the lock. Live SBF uses this exact vector. Shims prove
 * the CPI slices and the profit guard; they do not invent accounts.
 *
 * Alignment (same as CORE-007): Pump quote = SOL = DLMM Y;
 * Pump base = TOKEN = DLMM X.
 *
 *   0  SOL → DLMM → TOKEN → Pump → SOL
 *   1  SOL → Pump → TOKEN → DLMM → SOL
 *
 * OUR_EXEC is the invoked program id, not an account.
 */

#define ROUTE0_ID  0u

#define ROUTE0_DIR_DLMM_THEN_PUMP  0u
#define ROUTE0_DIR_PUMP_THEN_DLMM  1u

#define ROUTE0_ACC_AUTHORITY            0
#define ROUTE0_ACC_USER_QUOTE           1
#define ROUTE0_ACC_USER_BASE            2
#define ROUTE0_ACC_TOKEN_PROGRAM        3
#define ROUTE0_ACC_SYSTEM_PROGRAM       4
#define ROUTE0_ACC_ATA_PROGRAM          5
#define ROUTE0_ACC_MEMO_PROGRAM         6
#define ROUTE0_ACC_DLMM_PROGRAM         7
#define ROUTE0_ACC_DLMM_EVENT_AUTH      8
#define ROUTE0_ACC_DLMM_LB_PAIR         9
#define ROUTE0_ACC_DLMM_BITMAP         10
#define ROUTE0_ACC_DLMM_RESERVE_X      11
#define ROUTE0_ACC_DLMM_RESERVE_Y      12
#define ROUTE0_ACC_DLMM_ORACLE         13
#define ROUTE0_ACC_DLMM_HOST_FEE       14
#define ROUTE0_ACC_DLMM_TOKEN_X_MINT   15
#define ROUTE0_ACC_DLMM_TOKEN_Y_MINT   16
#define ROUTE0_ACC_DLMM_BIN_0          17
#define ROUTE0_ACC_DLMM_BIN_1          18
#define ROUTE0_ACC_PUMP_PROGRAM        19
#define ROUTE0_ACC_PUMP_EVENT_AUTH     20
#define ROUTE0_ACC_PUMP_POOL           21
#define ROUTE0_ACC_PUMP_GLOBAL         22
#define ROUTE0_ACC_PUMP_FEE_CONFIG     23
#define ROUTE0_ACC_PUMP_FEE_PROGRAM    24
#define ROUTE0_ACC_PUMP_BASE_MINT      25
#define ROUTE0_ACC_PUMP_QUOTE_MINT     26
#define ROUTE0_ACC_PUMP_POOL_BASE      27
#define ROUTE0_ACC_PUMP_POOL_QUOTE     28
#define ROUTE0_ACC_PUMP_PROTO_FEE      29
#define ROUTE0_ACC_PUMP_PROTO_FEE_ATA  30
#define ROUTE0_ACC_PUMP_CREATOR_ATA    31
#define ROUTE0_ACC_PUMP_CREATOR_AUTH   32
#define ROUTE0_ACC_PUMP_GLOBAL_VOL     33
#define ROUTE0_ACC_PUMP_USER_VOL       34

#define ROUTE0_N  35

#define ROUTE0_IX_LEN  25

/* "ARBEXEC0" */
#define ROUTE0_DISC  { 0x41, 0x52, 0x42, 0x45, 0x58, 0x45, 0x43, 0x30 }

#define ROUTE0_OK             0
#define ROUTE0_ERR_IX        -1
#define ROUTE0_ERR_ACCOUNTS  -2
#define ROUTE0_ERR_SIGNER    -3
#define ROUTE0_ERR_DLMM      -4
#define ROUTE0_ERR_PUMP      -5
#define ROUTE0_ERR_PROFIT    -6
#define ROUTE0_ERR_TOKEN     -7

#define EXEC_ACC_DATA  256
#define SPL_TOKEN_LEN  165
#define SPL_AMOUNT_OFF 64
#define SPL_MINT_OFF   0
#define SPL_OWNER_OFF  32

#define ROUTE0_CU_ENTRY     5000u
#define ROUTE0_CU_ACCOUNT    100u
#define ROUTE0_CU_CPI      15000u
#define ROUTE0_CU_TRANSFER  3000u
#define ROUTE0_CU_GUARD      500u

typedef struct {
    uint8_t  key[32];
    uint8_t  owner[32];
    uint64_t lamports;
    uint16_t dlen;
    uint8_t  writable;
    uint8_t  signer;
    uint8_t  data[EXEC_ACC_DATA];
} exec_account_t;

typedef struct {
    int      rc;
    uint64_t cu;
    uint64_t quote_before;
    uint64_t quote_after;
    uint64_t base_before;
    uint64_t base_after;
} route0_result_t;

extern const char *const route0_acc_name[ROUTE0_N];

int route0_pack(const opportunity_t *opp, uint64_t min_profit,
                uint8_t *buf, size_t cap, size_t *out_len);

int route0_process(exec_account_t *acc, uint32_t nacc,
                   const uint8_t *ix, size_t ixlen,
                   route0_result_t *out);

#endif /* ARB_EXEC_ROUTE0_H */
