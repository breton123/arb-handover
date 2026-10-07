#ifndef FIRM_WIRE_LUT_FILE_H
#define FIRM_WIRE_LUT_FILE_H

#include "wire/classify.h"

/*
 * Offline LUT warm. One jsonl object per line. Not an RPC client.
 */

int wire_lut_load_jsonl(wire_lut_tab_t *tab, const char *path);

typedef int (*wire_lut_jsonl_fn)(void *ctx, const uint8_t key[32],
                                 const uint8_t *addrs, uint32_t n);

int wire_lut_jsonl_each(const char *path, wire_lut_jsonl_fn fn, void *ctx);

#endif
