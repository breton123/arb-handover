#include "shred/rs.h"

#include <string.h>

static uint8_t g_exp[512];
static uint8_t g_log[256];
static int g_ok;

static void
gf_init(void)
{
    uint32_t x = 1, i;

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
    g_ok = 1;
}

static uint8_t
gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    return g_exp[(uint32_t)g_log[a] + (uint32_t)g_log[b]];
}

static uint8_t
gf_div(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    return g_exp[(uint32_t)g_log[a] + 255u - (uint32_t)g_log[b]];
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

static int
poly_from_points(const uint8_t *idx, uint8_t *const *src, uint32_t n,
                 uint32_t col, uint8_t *coef, const uint8_t *inv)
{
    uint8_t y[SHRED_RS_MAX];
    uint32_t i, u;

    for (i = 0; i < n; i++) {
        y[i] = src[i][col];
    }
    (void)idx;
    for (i = 0; i < n; i++) {
        uint8_t s = 0;
        for (u = 0; u < n; u++) {
            s ^= gf_mul(inv[i * n + u], y[u]);
        }
        coef[i] = s;
    }
    return 0;
}

static uint8_t
poly_eval(const uint8_t *coef, uint32_t n, uint8_t x)
{
    uint8_t acc = 0, p = 1;
    uint32_t i;

    for (i = 0; i < n; i++) {
        acc ^= gf_mul(coef[i], p);
        p = gf_mul(p, x);
    }
    return acc;
}

int
shred_rs_encode(uint8_t *const data[], uint8_t *coding[], uint32_t n,
                uint32_t k, uint32_t shard_len)
{
    uint8_t xv[SHRED_RS_MAX], inv[SHRED_RS_MAX * SHRED_RS_MAX];
    uint8_t *src[SHRED_RS_MAX];
    uint32_t i, j, col;

    if (n == 0 || n > SHRED_RS_MAX || k > SHRED_RS_MAX) {
        return -1;
    }
    gf_init();
    for (i = 0; i < n; i++) {
        if (data[i] == NULL) {
            return -1;
        }
        xv[i] = (uint8_t)i;
        src[i] = data[i];
    }
    if (vand_inv(xv, n, inv) != 0) {
        return -1;
    }
    for (j = 0; j < k; j++) {
        if (coding[j] == NULL) {
            return -1;
        }
    }
    for (col = 0; col < shard_len; col++) {
        uint8_t coef[SHRED_RS_MAX];

        poly_from_points(xv, src, n, col, coef, inv);
        for (j = 0; j < k; j++) {
            coding[j][col] = poly_eval(coef, n, (uint8_t)(n + j));
        }
    }
    return 0;
}

int
shred_rs_recover(uint8_t *shards[], const uint8_t *have, uint32_t n,
                 uint32_t k, uint32_t shard_len)
{
    uint8_t xv[SHRED_RS_MAX], inv[SHRED_RS_MAX * SHRED_RS_MAX];
    uint8_t *src[SHRED_RS_MAX];
    uint32_t n_have = 0, i, col, total;

    if (shards == NULL || have == NULL || n == 0 || n > SHRED_RS_MAX
        || k > SHRED_RS_MAX) {
        return -1;
    }
    total = n + k;
    gf_init();
    for (i = 0; i < total && n_have < n; i++) {
        if (have[i]) {
            if (shards[i] == NULL) {
                return -1;
            }
            xv[n_have] = (uint8_t)i;
            src[n_have] = shards[i];
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
    if (vand_inv(xv, n, inv) != 0) {
        return -1;
    }
    for (col = 0; col < shard_len; col++) {
        uint8_t coef[SHRED_RS_MAX];

        poly_from_points(xv, src, n, col, coef, inv);
        for (i = 0; i < total; i++) {
            if (!have[i] && shards[i] != NULL) {
                shards[i][col] = poly_eval(coef, n, (uint8_t)i);
            }
        }
    }
    return 0;
}
