#ifndef FIRM_SHRED_SHA256_H
#define FIRM_SHRED_SHA256_H

#include <stddef.h>
#include <stdint.h>

void shred_sha256(const uint8_t *data, size_t n, uint8_t out[32]);

#endif
