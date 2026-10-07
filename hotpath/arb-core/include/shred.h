#ifndef ARB_CORE_SHRED_H
#define ARB_CORE_SHRED_H

#include <stdint.h>
#include <string.h>

/* Same wire layout as arb-nic. Frozen parse. */
#define SHRED_COMMON_HDR_SZ  83u
#define SHRED_DATA_HDR_SZ    88u
#define SHRED_CODE_HDR_SZ    89u
#define SHRED_MAX_SZ         1228u

#define SHRED_OFF_VARIANT    64u
#define SHRED_OFF_SLOT       65u
#define SHRED_OFF_INDEX      73u
#define SHRED_OFF_VERSION    77u
#define SHRED_OFF_FEC        79u

#define SHRED_TYPE_LEGACY_CODE    0x50u
#define SHRED_TYPE_MERKLE_CODE    0x40u
#define SHRED_TYPE_CHAINED_CODE   0x60u
#define SHRED_TYPE_RESIGNED_CODE  0x70u
#define SHRED_TYPE_LEGACY_DATA    0xA0u
#define SHRED_TYPE_MERKLE_DATA    0x80u
#define SHRED_TYPE_CHAINED_DATA   0x90u
#define SHRED_TYPE_RESIGNED_DATA  0xB0u

typedef struct shred_view {
    const uint8_t *payload;
    uint64_t slot;
    uint32_t index;
    uint32_t fec_set;
    uint16_t version;
    uint8_t  type;
} shred_view_t;

static inline uint16_t shred_load_u16_le(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint32_t shred_load_u32_le(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint64_t shred_load_u64_le(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline int shred_is_data(uint8_t type)
{
    uint8_t t = (uint8_t)(type & 0xF0u);
    return t == SHRED_TYPE_LEGACY_DATA || t == SHRED_TYPE_MERKLE_DATA
        || t == SHRED_TYPE_CHAINED_DATA || t == SHRED_TYPE_RESIGNED_DATA;
}

static inline int shred_is_code(uint8_t type)
{
    uint8_t t = (uint8_t)(type & 0xF0u);
    return t == SHRED_TYPE_LEGACY_CODE || t == SHRED_TYPE_MERKLE_CODE
        || t == SHRED_TYPE_CHAINED_CODE || t == SHRED_TYPE_RESIGNED_CODE;
}

static inline uint16_t shred_header_sz(uint8_t type)
{
    return shred_is_data(type) ? (uint16_t)SHRED_DATA_HDR_SZ
         : shred_is_code(type) ? (uint16_t)SHRED_CODE_HDR_SZ
         : 0;
}

static inline int
shred_parse(const uint8_t *data, uint16_t len, shred_view_t *out)
{
    uint8_t type;
    uint16_t hdr;

    if (data == NULL || len < SHRED_COMMON_HDR_SZ || len > SHRED_MAX_SZ) {
        return -1;
    }
    type = data[SHRED_OFF_VARIANT];
    hdr = shred_header_sz(type);
    if (hdr == 0 || len < hdr) {
        return -1;
    }
    out->payload = data + hdr;
    out->slot = shred_load_u64_le(data + SHRED_OFF_SLOT);
    out->index = shred_load_u32_le(data + SHRED_OFF_INDEX);
    out->fec_set = shred_load_u32_le(data + SHRED_OFF_FEC);
    out->version = shred_load_u16_le(data + SHRED_OFF_VERSION);
    out->type = type;
    return 0;
}

#endif /* ARB_CORE_SHRED_H */
