#ifndef ARB_CORE_CLASSIFY_H
#define ARB_CORE_CLASSIFY_H

#include <stdint.h>
#include <string.h>

/*
 * Exact 32-byte program-id scan. Two needles.
 * LOCKED: AVX2 is the production reject path. Do not retune.
 *
 *   Meteora DLMM  LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo
 *   PumpSwap      pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA
 *
 * Discriminator bytes (CLASSIFY-002): offset +0 and +7.
 */

#define REL_NONE  0u
#define REL_DLMM  (1u << 0)
#define REL_PUMP  (1u << 1)
#define REL_BOTH  (REL_DLMM | REL_PUMP)

#define CLASSIFY_DISC0  0
#define CLASSIFY_DISC1  7

static const uint8_t PROG_DLMM[32] = {
    0x04, 0xe9, 0xe1, 0x2f, 0xbc, 0x84, 0xe8, 0x26,
    0xc9, 0x32, 0xcc, 0xe9, 0xe2, 0x64, 0x0c, 0xce,
    0x15, 0x59, 0x0c, 0x1c, 0x62, 0x73, 0xb0, 0x92,
    0x57, 0x08, 0xba, 0x3b, 0x85, 0x20, 0xb0, 0xbc
};

static const uint8_t PROG_PUMP[32] = {
    0x0c, 0x14, 0xde, 0xfc, 0x82, 0x5e, 0xc6, 0x76,
    0x94, 0x25, 0x08, 0x18, 0xbb, 0x65, 0x40, 0x65,
    0xf4, 0x29, 0x8d, 0x31, 0x56, 0xd5, 0x71, 0xb4,
    0xd4, 0xf8, 0x09, 0x0c, 0x18, 0xe9, 0xa8, 0x63
};

static inline int id_eq32(const uint8_t *p, const uint8_t *id)
{
    return memcmp(p, id, 32) == 0;
}

typedef uint32_t (*classify_fn)(const uint8_t *payload, uint16_t len);

uint32_t classify_scalar(const uint8_t *payload, uint16_t len);
uint32_t classify_avx2(const uint8_t *payload, uint16_t len);
uint32_t classify_avx512(const uint8_t *payload, uint16_t len);

int classify_have_avx2(void);
int classify_have_avx512(void);

#endif /* ARB_CORE_CLASSIFY_H */
