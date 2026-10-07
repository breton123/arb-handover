#ifndef FIRM_WIRE_PIPELINE_H
#define FIRM_WIRE_PIPELINE_H

#include "ingress/tx.h"
#include "state/compact.h"
#include "transitions/apply.h"

#include <stdint.h>

/*
 * WIRE-TO-STATE-001. Packet → certified publish. No DLMM. No RPC.
 *
 * Stages are TSC deltas from tsc_rx. tsc_pre_ready is when AUTH
 * (V/fees/vaults/mints) was certified; 0 means ready at rx.
 */

enum {
    WIRE_ST_PACKET = 0,
    WIRE_ST_TX_COMPLETE,
    WIRE_ST_ORDERED,
    WIRE_ST_RESOLVE,
    WIRE_ST_CLASSIFY,
    WIRE_ST_PRESTATE,
    WIRE_ST_APPLY,
    WIRE_ST_CERT,
    WIRE_ST_PUBLISH,
    WIRE_ST_E2E,
    WIRE_ST_N
};

#define WIRE_FAIL_NONE         0u
#define WIRE_FAIL_SHRED        1u
#define WIRE_FAIL_TX_WAIT      2u
#define WIRE_FAIL_DECODE       3u
#define WIRE_FAIL_ALT          4u
#define WIRE_FAIL_RESOLVE      5u
#define WIRE_FAIL_PRE_VQ       6u
#define WIRE_FAIL_PRE_FEE      7u
#define WIRE_FAIL_PRE_VAULT    8u
#define WIRE_FAIL_PRE_MINT     9u
#define WIRE_FAIL_FALLBACK     10u
#define WIRE_FAIL_UNKNOWN      11u
#define WIRE_FAIL_APPLY        12u
#define WIRE_FAIL_DLMM         13u

typedef struct {
    const uint8_t *pkt;
    uint16_t       pkt_len;
    const uint8_t *framed;     /* optional: skip shred extract */
    uint32_t       framed_len;
    uint64_t       tsc_rx;         /* first useful packet */
    uint64_t       tsc_tx_ready;   /* 0 ⇒ measured now */
    uint64_t       tsc_pre_ready;  /* 0 ⇒ ready at first packet */
    compact_state_t *st;
} wire_in_t;

typedef struct {
    uint64_t     ns[WIRE_ST_N];
    uint64_t     tsc_tx_ready;
    uint64_t     tsc_pre_ready;
    uint64_t     tsc_publish;
    uint8_t      admit;
    uint8_t      fail;
    uint8_t      tx_before_pre;
    uint8_t      pre_complete;
    int          apply_rc;
    ordered_tx_t tx;
    apply_result_t ar;
} wire_out_t;

int wire_to_state(const wire_in_t *in, wire_out_t *out,
                  double tsc_hz);

const char *wire_fail_name(uint8_t fail);

#endif /* FIRM_WIRE_PIPELINE_H */
