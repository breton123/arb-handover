#include "state/boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int
unhex(const char *s, uint8_t *out, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        int a, b;

        a = hexval(s[2u * i]);
        b = hexval(s[2u * i + 1u]);
        if (a < 0 || b < 0) {
            return -1;
        }
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static const char *
json_qstr(const char *line, const char *key)
{
    const char *p = strstr(line, key);

    if (p == NULL) {
        return NULL;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return NULL;
    }
    return p + 1;
}

static int
json_u64(const char *line, const char *key, uint64_t *out)
{
    const char *p = strstr(line, key);
    char *end;
    unsigned long long v;

    if (p == NULL || out == NULL) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    v = strtoull(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int
json_i64(const char *line, const char *key, int64_t *out)
{
    const char *p = strstr(line, key);
    char *end;
    long long v;

    if (p == NULL || out == NULL) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    v = strtoll(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

int
boot_load_jsonl(compact_state_t *st, const char *path)
{
    FILE *f;
    char line[4096];

    if (st == NULL || path == NULL) {
        return -1;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        const char *ps, *vbs, *vqs;
        uint8_t pool[32], vb[32], vq[32];
        uint8_t *vbp = NULL, *vqp = NULL;
        pump_state_t s;
        uint64_t slot = 0, u;

        ps = json_qstr(line, "\"pool_hex\"");
        if (ps == NULL || unhex(ps, pool, 32) != 0) {
            continue;
        }
        memset(&s, 0, sizeof(s));
        if (json_u64(line, "\"reserve_base\"", &s.reserve_base) != 0
            || json_u64(line, "\"reserve_quote\"", &s.reserve_quote) != 0) {
            continue;
        }
        if (json_i64(line, "\"virtual_quote\"", &s.virtual_quote) != 0) {
            s.virtual_quote = 0;
        }
        if (json_u64(line, "\"lp_fee_bps\"", &s.lp_fee_bps) != 0) {
            s.lp_fee_bps = 0;
        }
        if (json_u64(line, "\"protocol_fee_bps\"", &s.protocol_fee_bps) != 0) {
            s.protocol_fee_bps = 0;
        }
        if (json_u64(line, "\"creator_fee_bps\"", &s.creator_fee_bps) != 0) {
            s.creator_fee_bps = 0;
        }
        if (json_u64(line, "\"disabled\"", &u) == 0) {
            s.disabled = (uint8_t)u;
        }
        (void)json_u64(line, "\"slot\"", &slot);
        {
            const char *cs = json_qstr(line, "\"commitment\"");

            if (cs == NULL || strncmp(cs, "finalized", 9) != 0) {
                continue;
            }
        }
        if (slot == 0) {
            continue;
        }
        vbs = json_qstr(line, "\"vault_base_hex\"");
        if (vbs != NULL && unhex(vbs, vb, 32) == 0) {
            vbp = vb;
        }
        vqs = json_qstr(line, "\"vault_quote_hex\"");
        if (vqs != NULL && unhex(vqs, vq, 32) == 0) {
            vqp = vq;
        }
        (void)compact_pool_arm(st, pool, vbp, vqp, &s, slot, NULL);
    }
    fclose(f);
    return 0;
}
