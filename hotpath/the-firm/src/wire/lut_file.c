#include "wire/lut_file.h"

#include <stdio.h>
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
find_key(const char *line, const char *key)
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

int
wire_lut_jsonl_each(const char *path, wire_lut_jsonl_fn fn, void *ctx)
{
    FILE *f;
    char line[98304];
    int rc = 0;

    if (path == NULL || fn == NULL) {
        return -1;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        const char *ks, *as;
        uint8_t key[32], addrs[256][32];
        uint32_t n = 0;

        ks = find_key(line, "\"key_hex\"");
        if (ks == NULL || unhex(ks, key, 32) != 0) {
            continue;
        }
        as = strstr(line, "\"addrs_hex\"");
        if (as != NULL) {
            const char *q = strchr(as, '[');

            if (q != NULL) {
                q++;
                while (n < 256u) {
                    while (*q == ' ' || *q == ',' || *q == '\n') {
                        q++;
                    }
                    if (*q == ']' || *q == 0) {
                        break;
                    }
                    if (*q != '"') {
                        break;
                    }
                    q++;
                    if (unhex(q, addrs[n], 32) != 0) {
                        break;
                    }
                    n++;
                    q = strchr(q, '"');
                    if (q == NULL) {
                        break;
                    }
                    q++;
                }
            }
        }
        rc = fn(ctx, key, addrs[0], n);
        if (rc != 0) {
            break;
        }
    }
    fclose(f);
    return rc;
}

static int
load_put(void *ctx, const uint8_t key[32], const uint8_t *addrs, uint32_t n)
{
    (void)wire_lut_put((wire_lut_tab_t *)ctx, key, addrs, n);
    return 0;
}

int
wire_lut_load_jsonl(wire_lut_tab_t *tab, const char *path)
{
    if (tab == NULL || path == NULL) {
        return -1;
    }
    return wire_lut_jsonl_each(path, load_put, tab);
}
