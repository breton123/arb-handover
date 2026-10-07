#include "authpub.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint8_t *g_map;
static size_t g_len;
static uint32_t g_cap;

static uint32_t
u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t
u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static int
header_ok(void)
{
    if (g_map == NULL || g_len < AUTH_HDR) {
        return 0;
    }
    if (u32(g_map) != AUTH_MAGIC) {
        return 0;
    }
    if (u32(g_map + 4) != ((uint32_t)AUTH_RING << 16 | AUTH_VER)) {
        return 0;
    }
    if (u32(g_map + 16) != AUTH_BLOB) {
        return 0;
    }
    g_cap = u32(g_map + 12);
    if (g_cap == 0 || g_cap > 65535u) {
        return 0;
    }
    if (g_len < (size_t)AUTH_HDR + (size_t)g_cap * (size_t)AUTH_POOL) {
        return 0;
    }
    return 1;
}

int
authpub_open(const char *path)
{
    int fd;
    struct stat st;

    authpub_close();
    if (path == NULL) {
        path = AUTH_PATH_DEFAULT;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)AUTH_HDR) {
        close(fd);
        return -1;
    }
    g_len = (size_t)st.st_size;
    g_map = mmap(NULL, g_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (g_map == MAP_FAILED) {
        g_map = NULL;
        g_len = 0;
        return -1;
    }
    if (!header_ok()) {
        authpub_close();
        return -1;
    }
    return 0;
}

void
authpub_close(void)
{
    if (g_map != NULL && g_map != MAP_FAILED) {
        munmap(g_map, g_len);
    }
    g_map = NULL;
    g_len = 0;
    g_cap = 0;
}

int
authpub_ready(void)
{
    if (!header_ok()) {
        return 0;
    }
    return u32(g_map + 20) != 0u;
}

uint64_t
authpub_hash(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 14695981039346656037ull;
    size_t i;

    if (b == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static const uint8_t *
pool_base(uint32_t idx)
{
    if (!header_ok() || idx >= g_cap) {
        return NULL;
    }
    return g_map + AUTH_HDR + (size_t)idx * (size_t)AUTH_POOL;
}

static const uint8_t *
ent_at(const uint8_t *pool, uint32_t i)
{
    return pool + AUTH_POOL_HDR + (size_t)(i % AUTH_RING) * (size_t)AUTH_ENT;
}

static int
copy_ent(const uint8_t *e, authpub_meta_t *meta, uint8_t *blob, uint16_t *blob_len)
{
    uint16_t n;

    memset(meta, 0, sizeof(*meta));
    meta->generation = u64(e + 0);
    meta->slot = u64(e + 8);
    meta->txn_index = u32(e + 16);
    meta->flags = u32(e + 20);
    meta->state_version = u64(e + 24);
    memcpy(meta->sig, e + 32, 64);
    meta->proto = e[96];
    meta->coherent = e[97];
    n = (uint16_t)(e[98] | ((uint16_t)e[99] << 8));
    if (n > AUTH_BLOB) {
        return -1;
    }
    if (blob != NULL && n > 0) {
        memcpy(blob, e + 128, n);
    }
    if (blob_len != NULL) {
        *blob_len = n;
    }
    meta->hash = authpub_hash(e + 128, n);
    meta->ok = 1;
    return 0;
}

static int
usable(const uint8_t *e)
{
    uint32_t flags = u32(e + 20);
    if (e[97] == 0) {
        return 0;
    }
    if ((flags & AUTH_COHERENT) == 0) {
        return 0;
    }
    if ((flags & (AUTH_INCOMPLETE | AUTH_GAPPED)) != 0) {
        return 0;
    }
    return 1;
}

int
authpub_pred(uint32_t pool_idx, uint64_t trig_slot, const uint8_t sig[64],
             authpub_meta_t *meta, uint8_t *blob, uint16_t *blob_len)
{
    const uint8_t *pool;
    uint64_t s1, s2;
    uint32_t head, n, i, tries;
    int match = -1;

    if (meta == NULL) {
        return -1;
    }
    memset(meta, 0, sizeof(*meta));
    if (g_map == NULL && authpub_open(NULL) != 0) {
        return -1;
    }
    pool = pool_base(pool_idx);
    if (pool == NULL) {
        return -1;
    }
    for (tries = 0; tries < 64u; tries++) {
        s1 = u64(pool);
        if ((s1 & 1ull) != 0ull) {
            continue;
        }
        head = u32(pool + 8);
        n = head < AUTH_RING ? head : AUTH_RING;
        match = -1;
        for (i = 0; i < n; i++) {
            const uint8_t *e = ent_at(pool, head - 1u - i);
            if (!usable(e)) {
                continue;
            }
            if (sig != NULL && memcmp(e + 32, sig, 64) == 0) {
                match = (int)i;
                break;
            }
        }
        if (match >= 0) {
            /* N already published — take the previous usable entry. */
            for (i = (uint32_t)match + 1u; i < n; i++) {
                const uint8_t *e = ent_at(pool, head - 1u - i);
                if (usable(e) && copy_ent(e, meta, blob, blob_len) == 0) {
                    s2 = u64(pool);
                    if (s1 == s2 && (s2 & 1ull) == 0ull) {
                        return 0;
                    }
                    break;
                }
            }
        } else {
            for (i = 0; i < n; i++) {
                const uint8_t *e = ent_at(pool, head - 1u - i);
                uint64_t sl;
                if (!usable(e)) {
                    continue;
                }
                sl = u64(e + 8);
                if (sl < trig_slot && copy_ent(e, meta, blob, blob_len) == 0) {
                    s2 = u64(pool);
                    if (s1 == s2 && (s2 & 1ull) == 0ull) {
                        return 0;
                    }
                    break;
                }
            }
        }
        s2 = u64(pool);
        if (s1 == s2 && (s2 & 1ull) == 0ull) {
            return -1;
        }
    }
    return -1;
}
