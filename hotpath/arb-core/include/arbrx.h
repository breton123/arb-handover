#ifndef ARB_CORE_ARBRX_H
#define ARB_CORE_ARBRX_H

#include "shred.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARBRX_MAGIC     "ARBRX1"
#define ARBRX_MAGIC_LEN 8

typedef struct {
    const uint8_t *data;
    uint16_t len;
} arbrx_pkt_t;

static inline int
arbrx_load(const char *path, uint8_t **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t n;
    long sz;

    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < ARBRX_MAGIC_LEN || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz || memcmp(buf, ARBRX_MAGIC, 6) != 0) {
        free(buf);
        return -1;
    }
    *out_buf = buf;
    *out_len = n;
    return 0;
}

static inline int
arbrx_index(const uint8_t *buf, size_t len, arbrx_pkt_t **out, uint32_t *out_n)
{
    size_t off = ARBRX_MAGIC_LEN;
    uint32_t cap = 1024;
    uint32_t n = 0;
    arbrx_pkt_t *pkts = malloc((size_t)cap * sizeof(*pkts));

    if (pkts == NULL) {
        return -1;
    }
    while (off + 2 <= len) {
        uint16_t plen = shred_load_u16_le(buf + off);
        off += 2;
        if (plen == 0 || off + plen > len) {
            free(pkts);
            return -1;
        }
        if (n == cap) {
            arbrx_pkt_t *np = realloc(pkts, (size_t)cap * 2u * sizeof(*pkts));
            if (np == NULL) {
                free(pkts);
                return -1;
            }
            pkts = np;
            cap *= 2u;
        }
        pkts[n].data = buf + off;
        pkts[n].len = plen;
        n++;
        off += plen;
    }
    if (n == 0) {
        free(pkts);
        return -1;
    }
    *out = pkts;
    *out_n = n;
    return 0;
}

#endif /* ARB_CORE_ARBRX_H */
