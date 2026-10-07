#include "wire/lut_parse.h"

#include <string.h>

static uint32_t
u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static uint64_t
u64le(const uint8_t *p)
{
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

int
lut_parse_account(const uint8_t *data, uint32_t len, lut_account_t *out)
{
    uint32_t type, i, n, off;

    if (data == NULL || out == NULL || len < LUT_ADDR_OFF) {
        return -1;
    }
    type = u32le(data);
    if (type != 1u) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->deactivation_slot = u64le(data + 4);
    out->last_extended_slot = u64le(data + 12);
    out->start_index = data[20];
    out->have_meta = 1;
    off = LUT_ADDR_OFF;
    n = (len - off) / 32u;
    if (n > LUT_ADDR_MAX) {
        return -1;
    }
    if ((len - off) != n * 32u) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        memcpy(out->addr[i], data + off + i * 32u, 32);
    }
    out->n = (uint16_t)n;
    return 0;
}

int
lut_index_blocked(const lut_account_t *a, uint64_t slot, uint8_t idx)
{
    if (a == NULL || !a->have_meta || slot == LUT_SLOT_NEVER) {
        return 0;
    }
    if (a->deactivation_slot != LUT_SLOT_NEVER
        && slot > a->deactivation_slot) {
        return 1;
    }
    if (slot == a->last_extended_slot && idx >= a->start_index) {
        return 1;
    }
    return 0;
}
