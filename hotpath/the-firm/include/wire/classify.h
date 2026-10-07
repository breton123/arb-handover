#ifndef FIRM_WIRE_CLASSIFY_H
#define FIRM_WIRE_CLASSIFY_H

#include <stdint.h>

/*
 * WIRE-CLASSIFY-001. Semantic tags on reconstructed txs.
 * Does not apply Pump/DLMM. LUT miss is UNRESOLVED_ALT, not OTHER.
 */

#define WIRE_CL_OTHER           0u
#define WIRE_CL_DIRECT_PUMP     1u
#define WIRE_CL_DIRECT_DLMM     2u
#define WIRE_CL_KNOWN_ROUTER    3u
#define WIRE_CL_WATCHED         4u
#define WIRE_CL_UNRESOLVED_ALT  5u

#define WIRE_LUT_MAX 256u
#define WIRE_KEY_MAX 256u

typedef struct {
    uint8_t  used;
    uint16_t n;
    uint8_t  key[32];
    uint8_t  addr[256][32];
} wire_lut_t;

typedef struct {
    uint32_t n;
    wire_lut_t t[WIRE_LUT_MAX];
} wire_lut_tab_t;

typedef struct {
    uint8_t  klass;
    uint8_t  encoding;
    uint8_t  nalt;
    uint8_t  lut_resolved;
    uint8_t  lut_missing;
    uint16_t n_keys;
    uint16_t n_ix;
    uint8_t  sig[64];
} wire_class_t;

typedef struct {
    uint64_t verified;
    uint64_t direct_pump;
    uint64_t direct_dlmm;
    uint64_t known_router;
    uint64_t watched;
    uint64_t unresolved_alt;
    uint64_t other;
} wire_class_stats_t;

void wire_lut_tab_clear(wire_lut_tab_t *tab);
int wire_lut_put(wire_lut_tab_t *tab, const uint8_t key[32],
                 const uint8_t *addrs, uint32_t n);

int wire_classify(const uint8_t *tx, uint32_t len, const wire_lut_tab_t *luts,
                  wire_class_t *out);

#define WIRE_FAM_NONE      0u
#define WIRE_FAM_PUMP_AMM  1u
#define WIRE_FAM_PUMP_BOND 2u
#define WIRE_FAM_DLMM      3u
#define WIRE_IX_VIEW_MAX   32u

typedef struct {
    uint8_t  fam;
    uint8_t  prog_i;
    uint8_t  nacc;
    uint32_t dlen;
    uint8_t  disc[8];
    uint8_t  prog[32];
} wire_ix_view_t;

int wire_tx_ixs(const uint8_t *tx, uint32_t len, wire_ix_view_t *ix,
                uint32_t cap, uint32_t *n);

#endif
