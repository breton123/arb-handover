#ifndef FIRM_SHRED_RS_H
#define FIRM_SHRED_RS_H

#include <stdint.h>

#define SHRED_RS_MAX 67u

void shred_rs_init(void);

/*
 * Solana FEC: GF(2^8) 0x11d. Data eval 0..n-1, coding n..n+k-1.
 * have[i]=1 if shards[i] is valid. Missing data 0..n-1 must have
 * writable buffers. Missing coding may be NULL.
 */
int shred_rs_recover(uint8_t *shards[], const uint8_t *have, uint32_t n,
                     uint32_t k, uint32_t shard_len);

int shred_rs_encode(uint8_t *const data[], uint8_t *coding[], uint32_t n,
                    uint32_t k, uint32_t shard_len);

uint64_t shred_rs_cache_hits(void);
uint64_t shred_rs_cache_misses(void);

#endif
