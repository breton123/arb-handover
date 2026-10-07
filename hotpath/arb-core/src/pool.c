#include "pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_MAGIC "ARBPL1\0\0"

static uint32_t
key_hash(const uint8_t *k)
{
    uint64_t x;

    memcpy(&x, k, sizeof(x));
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    return (uint32_t)x;
}

static uint32_t
next_pow2(uint32_t n)
{
    if (n < 8) {
        return 8;
    }
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

int
pool_table_init(pool_table_t *t, uint32_t min_cap)
{
    uint32_t cap = next_pow2(min_cap);

    t->slot = calloc(cap, sizeof(*t->slot));
    if (t->slot == NULL) {
        return -1;
    }
    t->cap = cap;
    t->n = 0;
    return 0;
}

void
pool_table_free(pool_table_t *t)
{
    free(t->slot);
    t->slot = NULL;
    t->cap = 0;
    t->n = 0;
}

static int
grow(pool_table_t *t)
{
    pool_table_t ntbl;
    uint32_t i;

    if (pool_table_init(&ntbl, t->cap * 2u) != 0) {
        return -1;
    }
    ntbl.n = 0;
    for (i = 0; i < t->cap; i++) {
        if (t->slot[i].protocol != PROTO_NONE) {
            uint32_t h = key_hash(t->slot[i].key);
            uint32_t mask = ntbl.cap - 1;
            uint32_t j = h & mask;
            while (ntbl.slot[j].protocol != PROTO_NONE) {
                j = (j + 1u) & mask;
            }
            ntbl.slot[j] = t->slot[i];
            ntbl.n++;
        }
    }
    free(t->slot);
    *t = ntbl;
    return 0;
}

int
pool_table_put(pool_table_t *t, const uint8_t *key, uint8_t protocol)
{
    uint32_t h;
    uint32_t mask;
    uint32_t i;

    if (t->slot == NULL || key == NULL || protocol == PROTO_NONE) {
        return -1;
    }
    if (t->n * 2u >= t->cap) {
        if (grow(t) != 0) {
            return -1;
        }
    }
    h = key_hash(key);
    mask = t->cap - 1;
    i = h & mask;
    for (;;) {
        if (t->slot[i].protocol == PROTO_NONE) {
            memcpy(t->slot[i].key, key, 32);
            t->slot[i].protocol = protocol;
            t->slot[i].idx = t->n;
            t->n++;
            return 0;
        }
        if (t->slot[i].protocol == protocol && memcmp(t->slot[i].key, key, 32) == 0) {
            return 1;
        }
        i = (i + 1u) & mask;
    }
}

int
pool_table_get(const pool_table_t *t, const uint8_t *key, affected_pool_t *out)
{
    uint32_t h;
    uint32_t mask;
    uint32_t i;

    if (t->slot == NULL || key == NULL) {
        return -1;
    }
    h = key_hash(key);
    mask = t->cap - 1;
    i = h & mask;
    for (;;) {
        if (t->slot[i].protocol == PROTO_NONE) {
            return -1;
        }
        if (memcmp(t->slot[i].key, key, 32) == 0) {
            if (out != NULL) {
                out->protocol = t->slot[i].protocol;
                out->pool_idx = t->slot[i].idx;
            }
            return 0;
        }
        i = (i + 1u) & mask;
    }
}

int
pool_table_save(const pool_table_t *t, const char *path)
{
    FILE *f;
    uint32_t i;
    uint32_t written = 0;

    f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (fwrite(POOL_MAGIC, 1, 8, f) != 8) {
        fclose(f);
        return -1;
    }
    if (fwrite(&t->n, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < t->cap && written < t->n; i++) {
        if (t->slot[i].protocol == PROTO_NONE) {
            continue;
        }
        if (fwrite(&t->slot[i].protocol, 1, 1, f) != 1
            || fwrite(&t->slot[i].idx, 4, 1, f) != 1
            || fwrite(t->slot[i].key, 1, 32, f) != 32) {
            fclose(f);
            return -1;
        }
        written++;
    }
    fclose(f);
    return 0;
}

int
pool_table_load(pool_table_t *t, const char *path)
{
    FILE *f;
    char magic[8];
    uint32_t n;
    uint32_t i;

    f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, POOL_MAGIC, 6) != 0) {
        fprintf(stderr, "%s: not ARBPL1\n", path);
        fclose(f);
        return -1;
    }
    if (fread(&n, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    if (pool_table_init(t, n * 2u + 8u) != 0) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < n; i++) {
        uint8_t proto;
        uint32_t idx;
        uint8_t key[32];
        uint32_t h;
        uint32_t mask;
        uint32_t j;

        if (fread(&proto, 1, 1, f) != 1 || fread(&idx, 4, 1, f) != 1
            || fread(key, 1, 32, f) != 32 || proto == PROTO_NONE) {
            pool_table_free(t);
            fclose(f);
            return -1;
        }
        if (t->n * 2u >= t->cap && grow(t) != 0) {
            pool_table_free(t);
            fclose(f);
            return -1;
        }
        h = key_hash(key);
        mask = t->cap - 1;
        j = h & mask;
        while (t->slot[j].protocol != PROTO_NONE) {
            j = (j + 1u) & mask;
        }
        memcpy(t->slot[j].key, key, 32);
        t->slot[j].protocol = proto;
        t->slot[j].idx = idx;
        t->n++;
    }
    fclose(f);
    return 0;
}

static int
lookup_key_list(const msg_keys_t *mk, const pool_table_t *t, affected_pool_t *out)
{
    uint16_t i;

    for (i = 0; i < mk->nkeys; i++) {
        if (pool_table_get(t, mk->keys + (uint32_t)i * 32u, out) == 0) {
            return 0;
        }
    }
    return -1;
}

int
affected_from_payload(const uint8_t *payload, uint16_t len,
                      const pool_table_t *t, affected_pool_t *out)
{
    uint16_t prog_off;
    uint8_t proto;
    msg_keys_t mk;
    pool_cand_t cand;
    int framed;

    if (find_prog_id(payload, len, &prog_off, &proto) != 0) {
        return 2;
    }
    if (msg_keys_from_prog(payload, len, prog_off, &mk) != 0) {
        return 2;
    }
    framed = msg_first_dex_pool(payload, len, &mk, &cand);
    if (framed == 0) {
        if (pool_table_get(t, cand.key, out) == 0) {
            return 0;
        }
        if (lookup_key_list(&mk, t, out) == 0) {
            return 0;
        }
        return 1;
    }
    if (lookup_key_list(&mk, t, out) == 0) {
        return 0;
    }
    return 2;
}

int
affected_offset_scan(const uint8_t *payload, uint16_t len,
                     const pool_table_t *t, affected_pool_t *out)
{
    uint16_t prog_off = 0;
    uint8_t proto;
    int saw_framed = 0;
    uint16_t i;
    uint16_t last;

    if (payload == NULL || t == NULL) {
        return 2;
    }

    /* Extra program-id alignments (first one already failed CORE-001). */
    if (find_next_prog_id(payload, len, 0, &prog_off, &proto) == 0) {
        uint16_t next = (uint16_t)(prog_off + 1u);
        while (find_next_prog_id(payload, len, next, &prog_off, &proto) == 0) {
            msg_keys_t mk;
            pool_cand_t cand;

            if (msg_keys_from_prog(payload, len, prog_off, &mk) == 0) {
                if (msg_first_dex_pool(payload, len, &mk, &cand) == 0) {
                    saw_framed = 1;
                    if (pool_table_get(t, cand.key, out) == 0) {
                        return 0;
                    }
                }
                if (lookup_key_list(&mk, t, out) == 0) {
                    return 0;
                }
            }
            next = (uint16_t)(prog_off + 1u);
        }
    }

    /* Byte-offset legacy/V0 scan. Fail-fast on impossible sig counts. */
    if (len < 134) {
        return saw_framed ? 1 : 2;
    }
    last = (uint16_t)(len - 134);
    for (i = 0; i <= last; i++) {
        msg_keys_t mk;
        pool_cand_t cand;
        uint8_t nsig;

        nsig = payload[i];
        if (nsig < 1 || nsig > TX_SIG_MAX) {
            continue;
        }
        if (txn_dex_at(payload, len, i, &mk, &cand) == 0) {
            saw_framed = 1;
            if (pool_table_get(t, cand.key, out) == 0) {
                return 0;
            }
            if (lookup_key_list(&mk, t, out) == 0) {
                return 0;
            }
        }
    }
    return saw_framed ? 1 : 2;
}
