#ifndef FIRM_SHRED_SHRED_H
#define FIRM_SHRED_SHRED_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Minimal Solana shred envelope. Proven offsets from arb-feed / arb-nic.
 *
 * This names a shred. It does not authenticate, recover FEC, or reconstruct
 * entries. payload aliases the caller's bytes.
 */

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

typedef struct {
    const uint8_t *payload;
    uint64_t       slot;
    uint32_t       index;
    uint32_t       fec_set;
    uint16_t       version;
    uint8_t        type;
} shred_view_t;

/*
 * Race key. Agave ShredId is (slot, index, ShredType::{Data,Code})
 * (ledger/src/shred.rs). The common header also carries fec_set_index
 * as a u32 and a shredding version u16. Two feed copies of the same
 * shred share all of those. Version is on the view, not the key:
 * it does not distinguish copies. shred_type is the wire variant byte
 * so Data vs Code (and merkle/chained/resigned) cannot collide.
 * Do not hash the payload.
 */
typedef struct {
    uint64_t slot;
    uint32_t index;
    uint32_t fec_set;
    uint8_t  shred_type;
} shred_identity_t;

typedef shred_identity_t shred_key_t;

static inline uint16_t
shred_load_u16_le(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint32_t
shred_load_u32_le(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint64_t
shred_load_u64_le(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline int
shred_is_data(uint8_t type)
{
    uint8_t t = (uint8_t)(type & 0xF0u);
    return t == SHRED_TYPE_LEGACY_DATA || t == SHRED_TYPE_MERKLE_DATA
        || t == SHRED_TYPE_CHAINED_DATA || t == SHRED_TYPE_RESIGNED_DATA;
}

static inline int
shred_is_code(uint8_t type)
{
    uint8_t t = (uint8_t)(type & 0xF0u);
    return t == SHRED_TYPE_LEGACY_CODE || t == SHRED_TYPE_MERKLE_CODE
        || t == SHRED_TYPE_CHAINED_CODE || t == SHRED_TYPE_RESIGNED_CODE;
}

static inline uint16_t
shred_header_sz(uint8_t type)
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

    if (data == NULL || out == NULL || len < SHRED_COMMON_HDR_SZ ||
        len > SHRED_MAX_SZ) {
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

/*
 * Locate a shred in a datagram. Offset 0 is the live UDP payload.
 * 8 / 28 / 42 cover 8-byte capture prefixes and IPv4/UDP headers.
 */
static inline int
shred_envelope(const uint8_t *pkt, uint16_t len, const uint8_t **out,
               uint16_t *olen)
{
    static const uint16_t offs[] = { 0, 8, 28, 42 };
    uint32_t i;

    if (pkt == NULL || out == NULL || olen == NULL) {
        return -1;
    }
    for (i = 0; i < 4u; i++) {
        shred_view_t v;
        uint16_t o = offs[i];
        uint16_t rem;

        if (len <= o) {
            continue;
        }
        rem = (uint16_t)(len - o);
        if (rem > SHRED_MAX_SZ) {
            rem = (uint16_t)SHRED_MAX_SZ;
        }
        if (shred_parse(pkt + o, rem, &v) == 0) {
            *out = pkt + o;
            *olen = rem;
            return 0;
        }
    }
    return -1;
}

static inline void
shred_identity_from_view(const shred_view_t *v, shred_identity_t *id)
{
    id->slot = v->slot;
    id->index = v->index;
    id->fec_set = v->fec_set;
    id->shred_type = v->type;
}

static inline int
shred_identify(const uint8_t *data, uint16_t len,
               shred_view_t *view, shred_identity_t *id)
{
    if (shred_parse(data, len, view) != 0) {
        return -1;
    }
    shred_identity_from_view(view, id);
    return 0;
}

static inline int
shred_identify_packet(const uint8_t *pkt, uint16_t len,
                      shred_view_t *view, shred_identity_t *id)
{
    const uint8_t *sp;
    uint16_t slen;

    if (shred_envelope(pkt, len, &sp, &slen) != 0) {
        return -1;
    }
    return shred_identify(sp, slen, view, id);
}

static inline int
shred_identity_eq(const shred_identity_t *a, const shred_identity_t *b)
{
    return a->slot == b->slot && a->index == b->index &&
           a->fec_set == b->fec_set && a->shred_type == b->shred_type;
}

#endif /* FIRM_SHRED_SHRED_H */
