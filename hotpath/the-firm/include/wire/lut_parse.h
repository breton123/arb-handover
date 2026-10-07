#ifndef FIRM_WIRE_LUT_PARSE_H
#define FIRM_WIRE_LUT_PARSE_H

#include <stdint.h>

/*
 * Address lookup table account body from getAccountInfo.
 * Not getAddressLookupTable JSON-RPC (that method does not exist).
 *
 * Layout matches solana-web3.js AddressLookupTableAccount.deserialize:
 * LOOKUP_TABLE_META_SIZE is 56 from the start of the account (u32
 * ProgramState included). Addresses are raw 32-byte keys from offset 56.
 * Do not use offset 60: that drops the first four bytes of address 0
 * and makes (len-60) non-aligned, so every real table fails to parse.
 */

#define LUT_META_SIZE     56u
#define LUT_ADDR_OFF      LUT_META_SIZE
#define LUT_ADDR_MAX      256u
#define LUT_SLOT_NEVER    (~(uint64_t)0)

typedef struct {
    uint64_t deactivation_slot;
    uint64_t last_extended_slot;
    uint8_t  start_index;
    uint8_t  have_meta;
    uint16_t n;
    uint8_t  addr[LUT_ADDR_MAX][32];
} lut_account_t;

int lut_parse_account(const uint8_t *data, uint32_t len, lut_account_t *out);

/* 0 = usable at `slot`; 1 = not yet active / deactivated. */
int lut_index_blocked(const lut_account_t *a, uint64_t slot, uint8_t idx);

#endif
