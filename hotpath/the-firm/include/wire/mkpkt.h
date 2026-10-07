#ifndef FIRM_WIRE_MKPKT_H
#define FIRM_WIRE_MKPKT_H

#include "shred/shred.h"

#include <stdint.h>
#include <string.h>

/* Test/bench packet builder. Not a production encoder. */

static const uint8_t WIRE_PUMP_PK[32] = {
    0x0c, 0x14, 0xde, 0xfc, 0x82, 0x5e, 0xc6, 0x76, 0x94, 0x25, 0x08, 0x18,
    0xbb, 0x65, 0x40, 0x65, 0xf4, 0x29, 0x8d, 0x31, 0x56, 0xd5, 0x71, 0xb4,
    0xd4, 0xf8, 0x09, 0x0c, 0x18, 0xe9, 0xa8, 0x63
};
static const uint8_t WIRE_SELL[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};

static uint32_t
wire_sv(uint8_t *p, uint32_t v)
{
    uint32_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        if (v) {
            b |= 0x80u;
        }
        p[n++] = b;
    } while (v);
    return n;
}

/*
 * Legacy Pump sell: keys[0]=pool, keys[1]=program. One ix, one account.
 * Returns tx length.
 */
static uint32_t
#if defined(__GNUC__)
__attribute__((unused))
#endif
wire_mk_pump_sell(uint8_t *buf, uint32_t cap, const uint8_t pool[32],
                  uint64_t ain)
{
    uint8_t *p = buf;
    uint32_t i;

    if (cap < 256) {
        return 0;
    }
    p += wire_sv(p, 1); /* nsig */
    memset(p, 0x11, 64);
    p += 64;
    *p++ = 1; /* num required */
    *p++ = 0;
    *p++ = 1; /* 1 readonly unsigned (program) */
    p += wire_sv(p, 2);
    memcpy(p, pool, 32);
    p += 32;
    memcpy(p, WIRE_PUMP_PK, 32);
    p += 32;
    memset(p, 0x22, 32); /* blockhash */
    p += 32;
    p += wire_sv(p, 1);
    *p++ = 1; /* program index */
    p += wire_sv(p, 1);
    *p++ = 0; /* pool account */
    p += wire_sv(p, 24);
    memcpy(p, WIRE_SELL, 8);
    p += 8;
    memcpy(p, &ain, 8);
    p += 8;
    memset(p, 0, 8);
    p += 8;
    i = (uint32_t)(p - buf);
    return i;
}

static uint16_t
#if defined(__GNUC__)
__attribute__((unused))
#endif
wire_wrap_shred(uint8_t *pkt, uint32_t cap, uint64_t slot,
                const uint8_t *tx, uint32_t txlen)
{
    uint16_t size;

    if (cap < SHRED_DATA_HDR_SZ + txlen || txlen > 0xffffu) {
        return 0;
    }
    memset(pkt, 0, SHRED_DATA_HDR_SZ + txlen);
    pkt[SHRED_OFF_VARIANT] = SHRED_TYPE_MERKLE_DATA;
    memcpy(pkt + SHRED_OFF_SLOT, &slot, 8);
    pkt[85] = 0x80u; /* DATA_COMPLETE */
    size = (uint16_t)(SHRED_DATA_HDR_SZ + txlen);
    memcpy(pkt + 86, &size, 2);
    memcpy(pkt + SHRED_DATA_HDR_SZ, tx, txlen);
    return (uint16_t)(SHRED_DATA_HDR_SZ + txlen);
}

#endif /* FIRM_WIRE_MKPKT_H */
