#include "shred/rs.h"

#include <string.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#define RS_CACHE 256u

static uint8_t g_exp[512];
static uint8_t g_log[256];
static uint8_t g_mul[256][256];
#if defined(__AVX2__)
static uint8_t g_lo[256][16];
static uint8_t g_hi[256][16];
#endif
static int g_ok;

static uint64_t g_hits;
static uint64_t g_misses;

typedef struct {
    uint64_t key;
    uint32_t n;
    uint32_t k;
    uint8_t  valid;
    uint8_t  n_pres;
    uint8_t  n_miss;
    uint8_t  pres[SHRED_RS_MAX];
    uint8_t  miss[SHRED_RS_MAX];
    uint8_t  R[SHRED_RS_MAX * SHRED_RS_MAX];
} rs_ent_t;

static rs_ent_t g_cache[RS_CACHE];

static void
gf_init(void)
{
    uint32_t x = 1, i, a, b;

    if (g_ok) {
        return;
    }
    g_log[0] = 0;
    for (i = 0; i < 255; i++) {
        g_exp[i] = (uint8_t)x;
        g_log[x] = (uint8_t)i;
        x <<= 1;
        if ((x & 0x100u) != 0) {
            x ^= 0x11du;
        }
    }
    for (i = 255; i < 512; i++) {
        g_exp[i] = g_exp[i - 255u];
    }
    for (a = 0; a < 256; a++) {
        g_mul[a][0] = 0;
        g_mul[0][a] = 0;
    }
    for (a = 1; a < 256; a++) {
        for (b = 1; b < 256; b++) {
            g_mul[a][b] = g_exp[(uint32_t)g_log[a] + (uint32_t)g_log[b]];
        }
    }
#if defined(__AVX2__)
    for (a = 0; a < 256; a++) {
        for (i = 0; i < 16; i++) {
            g_lo[a][i] = g_mul[a][i];
            g_hi[a][i] = g_mul[a][(uint32_t)i << 4];
        }
    }
#endif
    g_ok = 1;
}

void
shred_rs_init(void)
{
    gf_init();
}

uint64_t
shred_rs_cache_hits(void)
{
    return g_hits;
}

uint64_t
shred_rs_cache_misses(void)
{
    return g_misses;
}

static uint8_t
gf_mul(uint8_t a, uint8_t b)
{
    return g_mul[a][b];
}

static uint8_t
gf_div(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    return g_exp[(uint32_t)g_log[a] + 255u - (uint32_t)g_log[b]];
}

static void
gf_axpy(uint8_t *dst, const uint8_t *src, uint8_t c, uint32_t n)
{
    uint32_t i;

    if (c == 0) {
        return;
    }
    if (c == 1) {
        i = 0;
#if defined(__AVX2__)
        for (; i + 32u <= n; i += 32u) {
            __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
            __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
            _mm256_storeu_si256((__m256i *)(dst + i), _mm256_xor_si256(d, s));
        }
#endif
        for (; i < n; i++) {
            dst[i] ^= src[i];
        }
        return;
    }
    i = 0;
#if defined(__AVX2__)
    {
        __m128i tlo = _mm_loadu_si128((const __m128i *)g_lo[c]);
        __m128i thi = _mm_loadu_si128((const __m128i *)g_hi[c]);
        __m128i m4 = _mm_set1_epi8(0x0f);
        __m128i hf = _mm_set1_epi8((char)0xf0);

        for (; i + 16u <= n; i += 16u) {
            __m128i s = _mm_loadu_si128((const __m128i *)(src + i));
            __m128i d = _mm_loadu_si128((const __m128i *)(dst + i));
            __m128i lo = _mm_and_si128(s, m4);
            __m128i hi = _mm_srli_epi16(_mm_and_si128(s, hf), 4);
            __m128i p = _mm_xor_si128(_mm_shuffle_epi8(tlo, lo),
                                      _mm_shuffle_epi8(thi, hi));
            _mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(d, p));
        }
    }
#endif
    {
        const uint8_t *t = g_mul[c];
        for (; i < n; i++) {
            dst[i] ^= t[src[i]];
        }
    }
}

static int
vand_inv(const uint8_t *x, uint32_t n, uint8_t *inv)
{
    uint8_t a[SHRED_RS_MAX * SHRED_RS_MAX];
    uint32_t r, c, k;

    memset(inv, 0, (size_t)n * n);
    memset(a, 0, (size_t)n * n);
    for (r = 0; r < n; r++) {
        uint8_t p = 1;
        for (c = 0; c < n; c++) {
            a[r * n + c] = p;
            p = gf_mul(p, x[r]);
        }
        inv[r * n + r] = 1;
    }
    for (k = 0; k < n; k++) {
        uint32_t piv = k;
        uint8_t d;

        for (r = k; r < n; r++) {
            if (a[r * n + k] != 0) {
                piv = r;
                break;
            }
        }
        if (a[piv * n + k] == 0) {
            return -1;
        }
        if (piv != k) {
            for (c = 0; c < n; c++) {
                uint8_t t = a[k * n + c];
                a[k * n + c] = a[piv * n + c];
                a[piv * n + c] = t;
                t = inv[k * n + c];
                inv[k * n + c] = inv[piv * n + c];
                inv[piv * n + c] = t;
            }
        }
        d = a[k * n + k];
        for (c = 0; c < n; c++) {
            a[k * n + c] = gf_div(a[k * n + c], d);
            inv[k * n + c] = gf_div(inv[k * n + c], d);
        }
        for (r = 0; r < n; r++) {
            uint8_t f;
            if (r == k) {
                continue;
            }
            f = a[r * n + k];
            if (f == 0) {
                continue;
            }
            for (c = 0; c < n; c++) {
                a[r * n + c] ^= gf_mul(f, a[k * n + c]);
                inv[r * n + c] ^= gf_mul(f, inv[k * n + c]);
            }
        }
    }
    return 0;
}

static uint64_t
have_key(const uint8_t *have, uint32_t total)
{
    uint64_t m = 0;
    uint32_t i;

    for (i = 0; i < total && i < 64u; i++) {
        if (have[i]) {
            m |= 1ull << i;
        }
    }
    return m;
}

static int
build_R(const uint8_t *pres, uint32_t np, const uint8_t *miss, uint32_t nm,
        uint8_t *R)
{
    uint8_t inv[SHRED_RS_MAX * SHRED_RS_MAX];
    uint32_t a, j, t;

    if (vand_inv(pres, np, inv) != 0) {
        return -1;
    }
    memset(R, 0, (size_t)nm * np);
    for (a = 0; a < nm; a++) {
        uint8_t x = miss[a], xp = 1, pows[SHRED_RS_MAX];

        for (t = 0; t < np; t++) {
            pows[t] = xp;
            xp = gf_mul(xp, x);
        }
        for (j = 0; j < np; j++) {
            uint8_t s = 0;
            for (t = 0; t < np; t++) {
                s ^= gf_mul(inv[t * np + j], pows[t]);
            }
            R[a * np + j] = s;
        }
    }
    return 0;
}

static rs_ent_t *
cache_get(uint64_t key, uint32_t n, uint32_t k, const uint8_t *have,
          uint32_t total)
{
    rs_ent_t *e = &g_cache[key & (RS_CACHE - 1u)];
    uint32_t i, np = 0, nm = 0;

    if (e->valid && e->key == key && e->n == n && e->k == k) {
        g_hits++;
        return e;
    }
    g_misses++;
    memset(e, 0, sizeof(*e));
    for (i = 0; i < total; i++) {
        if (have[i]) {
            if (np < n) {
                e->pres[np++] = (uint8_t)i;
            }
        } else if (nm < SHRED_RS_MAX) {
            e->miss[nm++] = (uint8_t)i;
        }
    }
    if (np < n) {
        return NULL;
    }
    if (build_R(e->pres, np, e->miss, nm, e->R) != 0) {
        return NULL;
    }
    e->key = key;
    e->n = n;
    e->k = k;
    e->n_pres = (uint8_t)np;
    e->n_miss = (uint8_t)nm;
    e->valid = 1;
    return e;
}

static int
apply_R(const rs_ent_t *e, uint8_t *shards[], uint32_t shard_len)
{
    uint32_t a, j, np;

    np = e->n_pres;
    for (a = 0; a < e->n_miss; a++) {
        uint32_t mi = e->miss[a];
        uint8_t *dst;

        if (shards[mi] == NULL) {
            continue;
        }
        dst = shards[mi];
        memset(dst, 0, shard_len);
        for (j = 0; j < np; j++) {
            uint8_t c = e->R[a * np + j];
            uint32_t pi = e->pres[j];
            if (c != 0 && shards[pi] != NULL) {
                gf_axpy(dst, shards[pi], c, shard_len);
            }
        }
    }
    return 0;
}

int
shred_rs_encode(uint8_t *const data[], uint8_t *coding[], uint32_t n,
                uint32_t k, uint32_t shard_len)
{
    uint8_t have[SHRED_RS_MAX * 2u];
    uint8_t *shards[SHRED_RS_MAX * 2u];
    uint32_t i, total;
    rs_ent_t *e;

    if (n == 0 || n > SHRED_RS_MAX || k > SHRED_RS_MAX) {
        return -1;
    }
    gf_init();
    total = n + k;
    memset(have, 0, total);
    memset(shards, 0, sizeof(shards[0]) * total);
    for (i = 0; i < n; i++) {
        if (data[i] == NULL) {
            return -1;
        }
        have[i] = 1;
        shards[i] = data[i];
    }
    for (i = 0; i < k; i++) {
        if (coding[i] == NULL) {
            return -1;
        }
        shards[n + i] = coding[i];
    }
    e = cache_get(((uint64_t)n << 32) | k, n, k, have, total);
    if (e == NULL) {
        return -1;
    }
    return apply_R(e, shards, shard_len);
}

int
shred_rs_recover(uint8_t *shards[], const uint8_t *have, uint32_t n,
                 uint32_t k, uint32_t shard_len)
{
    uint32_t i, total, n_have = 0;
    uint64_t key;
    rs_ent_t *e;

    if (shards == NULL || have == NULL || n == 0 || n > SHRED_RS_MAX
        || k > SHRED_RS_MAX) {
        return -1;
    }
    total = n + k;
    gf_init();
    for (i = 0; i < total; i++) {
        if (have[i]) {
            if (shards[i] == NULL) {
                return -1;
            }
            n_have++;
        }
    }
    if (n_have < n) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (!have[i] && shards[i] == NULL) {
            return -1;
        }
    }
    key = have_key(have, total) ^ (((uint64_t)n) << 48) ^ (((uint64_t)k) << 40);
    e = cache_get(key, n, k, have, total);
    if (e == NULL) {
        return -1;
    }
    return apply_R(e, shards, shard_len);
}
