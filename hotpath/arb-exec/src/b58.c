#include "b58.h"

#include <string.h>

static const char ALPH[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int
b58_decode(const char *s, uint8_t *out, size_t out_len)
{
    uint8_t buf[128];
    size_t i, j, pad, n;

    if (s == NULL || out == NULL || out_len == 0 || out_len > 64) {
        return -1;
    }
    pad = 0;
    while (s[pad] == '1') {
        pad++;
    }
    memset(buf, 0, sizeof(buf));
    n = 1;
    for (i = pad; s[i] != '\0'; i++) {
        const char *p = strchr(ALPH, s[i]);
        unsigned carry;

        if (p == NULL) {
            return -1;
        }
        carry = (unsigned)(p - ALPH);
        for (j = 0; j < n; j++) {
            carry += (unsigned)buf[j] * 58u;
            buf[j] = (uint8_t)(carry & 0xffu);
            carry >>= 8;
        }
        while (carry > 0u) {
            if (n >= sizeof(buf)) {
                return -1;
            }
            buf[n++] = (uint8_t)(carry & 0xffu);
            carry >>= 8;
        }
    }
    while (n > 0u && buf[n - 1u] == 0) {
        n--;
    }
    if (pad + n > out_len) {
        return -1;
    }
    memset(out, 0, out_len);
    for (j = 0; j < n; j++) {
        out[out_len - 1u - j] = buf[j];
    }
    return 0;
}

int
b58_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    uint8_t tmp[128];
    size_t i, j, n, pad;
    unsigned carry;

    if (in == NULL || out == NULL || in_len == 0 || in_len > 64) {
        return -1;
    }
    pad = 0;
    while (pad < in_len && in[pad] == 0) {
        pad++;
    }
    memset(tmp, 0, sizeof(tmp));
    n = 0;
    for (i = pad; i < in_len; i++) {
        carry = in[i];
        for (j = 0; j < n; j++) {
            carry += 256u * (unsigned)tmp[j];
            tmp[j] = (uint8_t)(carry % 58u);
            carry /= 58u;
        }
        while (carry > 0u) {
            if (n >= sizeof(tmp)) {
                return -1;
            }
            tmp[n++] = (uint8_t)(carry % 58u);
            carry /= 58u;
        }
    }
    if (pad + n + 1u > out_cap) {
        return -1;
    }
    for (i = 0; i < pad; i++) {
        out[i] = '1';
    }
    for (i = 0; i < n; i++) {
        out[pad + i] = ALPH[tmp[n - 1u - i]];
    }
    out[pad + n] = '\0';
    return 0;
}

int
b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    static const char B[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    uint32_t v;

    if (in == NULL || out == NULL) {
        return -1;
    }
    for (i = 0; i < in_len; i += 3) {
        size_t rem = in_len - i;
        v = ((uint32_t)in[i]) << 16;
        if (rem > 1) {
            v |= ((uint32_t)in[i + 1]) << 8;
        }
        if (rem > 2) {
            v |= (uint32_t)in[i + 2];
        }
        if (o + 4 >= out_cap) {
            return -1;
        }
        out[o++] = B[(v >> 18) & 63];
        out[o++] = B[(v >> 12) & 63];
        out[o++] = rem > 1 ? B[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? B[v & 63] : '=';
    }
    out[o] = '\0';
    return 0;
}
