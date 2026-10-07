#ifndef ARB_EXEC_B58_H
#define ARB_EXEC_B58_H

#include <stddef.h>
#include <stdint.h>

int b58_decode(const char *s, uint8_t *out, size_t out_len);
int b58_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

#endif /* ARB_EXEC_B58_H */
