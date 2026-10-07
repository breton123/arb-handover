#ifndef ARB_CORE_PROTO_H
#define ARB_CORE_PROTO_H

#include <stdint.h>
#include <string.h>

/*
 * CORE-009 — protocol ids beyond frozen DLMM/Pump.
 * 1 and 2 stay as in tx.h. Do not renumber.
 */

#ifndef PROTO_DLMM
#define PROTO_DLMM  1u
#endif
#ifndef PROTO_PUMP
#define PROTO_PUMP  2u
#endif
#define PROTO_CLMM  3u
#define PROTO_CPMM  4u
#define PROTO_DAMM  5u
#define PROTO_ORCA  6u
#define PROTO_N     7u

#define REL_N_NONE  0u
#define REL_N_DLMM  (1u << 0)
#define REL_N_PUMP  (1u << 1)
#define REL_N_CLMM  (1u << 2)
#define REL_N_CPMM  (1u << 3)
#define REL_N_DAMM  (1u << 4)
#define REL_N_ORCA  (1u << 5)
#define REL_N_ALL6  (REL_N_DLMM | REL_N_PUMP | REL_N_CLMM | REL_N_CPMM \
                     | REL_N_DAMM | REL_N_ORCA)

#define CLASSIFY_N_MAX  6u
#define CLASSIFY_N_DISC0 0
#define CLASSIFY_N_DISC1 7

/* Raydium CLMM  CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK */
static const uint8_t PROG_CLMM[32] = {
    0xa5, 0xd5, 0xca, 0x9e, 0x04, 0xcf, 0x5d, 0xb5,
    0x90, 0xb7, 0x14, 0xba, 0x2f, 0xe3, 0x2c, 0xb1,
    0x59, 0x13, 0x3f, 0xc1, 0xc1, 0x92, 0xb7, 0x22,
    0x57, 0xfd, 0x07, 0xd3, 0x9c, 0xb0, 0x40, 0x1e
};

/* Raydium CPMM  CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C */
static const uint8_t PROG_CPMM[32] = {
    0xa9, 0x2a, 0x5a, 0x8b, 0x4f, 0x29, 0x59, 0x52,
    0x84, 0x25, 0x50, 0xaa, 0x93, 0xfd, 0x5b, 0x95,
    0xb5, 0xac, 0xe6, 0xa8, 0xeb, 0x92, 0x0c, 0x93,
    0x94, 0x2e, 0x43, 0x69, 0x0c, 0x20, 0xec, 0x73
};

/* Meteora DAMM v2  cpamdpZCGKUy5JxQXB4dcpGPiikHawvSWAd6mEn1sGG */
static const uint8_t PROG_DAMM[32] = {
    0x09, 0x2d, 0x21, 0x35, 0x65, 0x7a, 0x15, 0x9c,
    0x2b, 0x87, 0xd4, 0xb6, 0x6a, 0x70, 0xdb, 0x8e,
    0x97, 0x52, 0x38, 0x9f, 0xf7, 0x6a, 0xaf, 0x20,
    0x6c, 0xed, 0x06, 0x3a, 0x38, 0xf9, 0x5a, 0xed
};

/* Orca Whirlpool  whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc */
static const uint8_t PROG_ORCA[32] = {
    0x0e, 0x03, 0x68, 0x5f, 0x8e, 0x90, 0x90, 0x53,
    0xe4, 0x58, 0x12, 0x1c, 0x66, 0xf5, 0xa7, 0x6a,
    0xed, 0xc7, 0x70, 0x6a, 0xa1, 0x1c, 0x82, 0xf8,
    0xaa, 0x95, 0x2a, 0x8f, 0x2b, 0x78, 0x79, 0xa9
};

/* wSOL  So11111111111111111111111111111111111111112 */
static const uint8_t MINT_SOL[32] = {
    0x06, 0x9b, 0x88, 0x57, 0xfe, 0xab, 0x81, 0x84,
    0xfb, 0x68, 0x7f, 0x63, 0x46, 0x18, 0xc0, 0x35,
    0xda, 0xc4, 0x39, 0xdc, 0x1a, 0xeb, 0x3b, 0x55,
    0x98, 0xa0, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x01
};

static inline int proto_id_eq(const uint8_t *p, const uint8_t *id)
{
    return memcmp(p, id, 32) == 0;
}

static inline uint8_t proto_from_prog(const uint8_t *key)
{
    if (proto_id_eq(key, PROG_CLMM)) {
        return PROTO_CLMM;
    }
    if (proto_id_eq(key, PROG_CPMM)) {
        return PROTO_CPMM;
    }
    if (proto_id_eq(key, PROG_DAMM)) {
        return PROTO_DAMM;
    }
    if (proto_id_eq(key, PROG_ORCA)) {
        return PROTO_ORCA;
    }
    return 0;
}

static inline uint32_t proto_rel_bit(uint8_t proto)
{
    switch (proto) {
    case 1:
        return REL_N_DLMM;
    case 2:
        return REL_N_PUMP;
    case PROTO_CLMM:
        return REL_N_CLMM;
    case PROTO_CPMM:
        return REL_N_CPMM;
    case PROTO_DAMM:
        return REL_N_DAMM;
    case PROTO_ORCA:
        return REL_N_ORCA;
    default:
        return REL_N_NONE;
    }
}

#endif /* ARB_CORE_PROTO_H */
