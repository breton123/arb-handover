#ifndef FIRM_STATE_BOOT_H
#define FIRM_STATE_BOOT_H

#include "state/compact.h"

/*
 * Trusted compact Pump snapshot jsonl → seed (ANCHOR-001 adapter output).
 *
 * jsonl keys: pool_hex, vault_base_hex, vault_quote_hex, slot
 * (that fetch's finalized RPC context.slot → row.anchor_slot),
 * commitment (must be finalized or the line is ignored),
 * reserve_base, reserve_quote, virtual_quote, lp_fee_bps,
 * protocol_fee_bps, creator_fee_bps, disabled.
 */

int boot_load_jsonl(compact_state_t *st, const char *path);

#endif
