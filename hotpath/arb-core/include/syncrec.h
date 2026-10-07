#ifndef ARB_CORE_SYNCREC_H
#define ARB_CORE_SYNCREC_H

#include "hot.h"
#include "live.h"
#include "swapix.h"

#include <stdint.h>
#include <stdio.h>

/*
 * Tomorrow's synchronized journal. RAW shreds stay FEEDCAP1.
 * This file is STATE + DECISIONS + EXEC beside that capture.
 *
 *   kind 1  STATE_INIT   compact S header (slot, version, n, pool keys)
 *   kind 2  MUTATION     committed N: before/after state_version
 *   kind 3  DECISION     trigger + version used + predicted S' + opp
 *   kind 4  EXEC         signed-ready / send / sig / result
 */

#define SYN_MAGIC      0x31434e59u /* "YNC1" */
#define SYN_VER        1u
#define SYN_KIND_INIT  1u
#define SYN_KIND_MUT   2u
#define SYN_KIND_DEC   3u
#define SYN_KIND_EXEC  4u

#define SYN_EXEC_NONE    0u
#define SYN_EXEC_READY   1u
#define SYN_EXEC_SENT    2u
#define SYN_EXEC_LAND    3u
#define SYN_EXEC_FAIL    4u

typedef struct {
    FILE    *f;
    uint64_t tsc_hz;
    uint64_t n_init;
    uint64_t n_mut;
    uint64_t n_dec;
    uint64_t n_exec;
} syncrec_t;

int syncrec_open(syncrec_t *s, const char *path, uint64_t tsc_hz);
void syncrec_close(syncrec_t *s);

int syncrec_state_init(syncrec_t *s, const live_univ_t *u, uint64_t ts_ns);

int syncrec_mutation(syncrec_t *s, uint64_t ts_ns,
                     uint64_t before_ver, uint64_t after_ver,
                     uint32_t pool_idx, const swapix_t *n);

int syncrec_decision(syncrec_t *s, uint64_t ts_ns,
                     const swapix_t *n, uint32_t pool_idx,
                     uint64_t state_version,
                     uint64_t pred_a, uint64_t pred_b,
                     const opportunity_t *opp);

int syncrec_exec(syncrec_t *s, uint64_t ts_ns,
                 const uint8_t tx_sig[64],
                 uint64_t signed_ready_ns, uint64_t send_ns,
                 uint8_t result);

#endif /* ARB_CORE_SYNCREC_H */
