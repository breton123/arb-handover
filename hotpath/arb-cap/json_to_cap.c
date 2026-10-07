/* Research-only. Not part of the arb-core hot path. */
#include "dlmm_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * stdin: one JSON object per line, fields as in record_dlmm.py
 * This file is compiled against arb-core headers only to pack dlmm_cap_rec_t.
 */

static char *
slurp(void)
{
    size_t cap = 1 << 20;
    size_t n = 0;
    char *b = malloc(cap);
    if (b == NULL) {
        return NULL;
    }
    for (;;) {
        size_t got = fread(b + n, 1, cap - n, stdin);
        n += got;
        if (got == 0) {
            break;
        }
        if (n == cap) {
            char *nb = realloc(b, cap * 2);
            if (nb == NULL) {
                free(b);
                return NULL;
            }
            b = nb;
            cap *= 2;
        }
    }
    b[n] = 0;
    return b;
}

/* Minimal field grabbers. Ugly on purpose. */
static int
js_u64(const char *o, const char *k, uint64_t *out)
{
    char pat[96];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", k);
    p = strstr(o, pat);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pat);
    while (*p == ' ') {
        p++;
    }
    if (*p == '"') {
        p++;
    }
    *out = strtoull(p, NULL, 10);
    return 0;
}

static int
js_i64(const char *o, const char *k, int64_t *out)
{
    uint64_t u;
    if (js_u64(o, k, &u) != 0) {
        return -1;
    }
    *out = (int64_t)u;
    return 0;
}

static int
js_i32(const char *o, const char *k, int32_t *out)
{
    int64_t v;
    if (js_i64(o, k, &v) != 0) {
        return -1;
    }
    *out = (int32_t)v;
    return 0;
}

static int
js_u32(const char *o, const char *k, uint32_t *out)
{
    uint64_t v;
    if (js_u64(o, k, &v) != 0) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int
js_u16(const char *o, const char *k, uint16_t *out)
{
    uint64_t v;
    if (js_u64(o, k, &v) != 0) {
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int
js_u8(const char *o, const char *k, uint8_t *out)
{
    uint64_t v;
    if (js_u64(o, k, &v) != 0) {
        return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

static const char *
js_arr(const char *o, const char *k)
{
    char pat[96];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", k);
    p = strstr(o, pat);
    if (p == NULL) {
        return NULL;
    }
    p = strchr(p, '[');
    return p;
}

static int
parse_bins(const char *arr, dlmm_hot_bin_t *bins, uint16_t *n)
{
    const char *p = arr;
    uint16_t i = 0;

    if (p == NULL) {
        *n = 0;
        return 0;
    }
    while (i < DLMM_CAP_BINS) {
        const char *obj = strchr(p, '{');
        if (obj == NULL || obj > strchr(arr, ']') /* weak */) {
            break;
        }
        {
            const char *end = strchr(obj, '}');
            if (end == NULL) {
                break;
            }
            if (js_i32(obj, "id", &bins[i].id) != 0
                || js_u64(obj, "x", &bins[i].amount_x) != 0
                || js_u64(obj, "y", &bins[i].amount_y) != 0) {
                return -1;
            }
            i++;
            p = end + 1;
            if (*p == ']') {
                break;
            }
        }
    }
    *n = i;
    return 0;
}

static int
fill_rec(const char *o, dlmm_cap_rec_t *r)
{
    int32_t delta;

    memset(r, 0, sizeof(*r));
    if (js_u32(o, "pool_idx", &r->pool_idx) != 0
        || js_u8(o, "swap_for_y", &r->swap_for_y) != 0
        || js_u64(o, "amount_in", &r->amount_in) != 0
        || js_u64(o, "min_out", &r->min_out) != 0
        || js_i64(o, "now_ts", &r->now_ts) != 0
        || js_i32(o, "before_active", &r->before_active) != 0
        || js_i32(o, "after_active", &r->after_active) != 0
        || js_u16(o, "bin_step", &r->bin_step) != 0
        || js_u8(o, "status", &r->status) != 0) {
        return -1;
    }
    js_u16(o, "base_factor", &r->parameters.base_factor);
    js_u16(o, "filter_period", &r->parameters.filter_period);
    js_u16(o, "decay_period", &r->parameters.decay_period);
    js_u16(o, "reduction_factor", &r->parameters.reduction_factor);
    js_u32(o, "variable_fee_control", &r->parameters.variable_fee_control);
    js_u32(o, "max_volatility_accumulator", &r->parameters.max_volatility_accumulator);
    js_u16(o, "protocol_share", &r->parameters.protocol_share);
    js_u8(o, "base_fee_power_factor", &r->parameters.base_fee_power_factor);
    js_u8(o, "collect_fee_mode", &r->parameters.collect_fee_mode);
    js_u32(o, "vol_acc_before", &r->v_before.volatility_accumulator);
    js_u32(o, "vol_ref_before", &r->v_before.volatility_reference);
    js_i32(o, "idx_ref_before", &r->v_before.index_reference);
    js_i64(o, "last_upd_before", &r->v_before.last_update_timestamp);
    js_u32(o, "vol_acc_after", &r->v_after.volatility_accumulator);
    js_u32(o, "vol_ref_after", &r->v_after.volatility_reference);
    js_i32(o, "idx_ref_after", &r->v_after.index_reference);
    js_i64(o, "last_upd_after", &r->v_after.last_update_timestamp);
    js_u64(o, "before_rx", &r->before_rx);
    js_u64(o, "before_ry", &r->before_ry);
    js_u64(o, "after_rx", &r->after_rx);
    js_u64(o, "after_ry", &r->after_ry);
    if (parse_bins(js_arr(o, "bins_before"), r->bins_before, &r->nbin_before) != 0) {
        return -1;
    }
    if (parse_bins(js_arr(o, "bins_after"), r->bins_after, &r->nbin_after) != 0) {
        return -1;
    }
    delta = r->after_active - r->before_active;
    if (delta < 0) {
        delta = -delta;
    }
    r->crossed = (uint8_t)delta;
    return 0;
}

int
main(int argc, char **argv)
{
    const char *out_path;
    char *raw;
    char *line;
    dlmm_cap_rec_t *recs = NULL;
    uint32_t n = 0;
    uint32_t cap = 0;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: json_to_cap out.cap < triples.jsonl\n");
        return 1;
    }
    out_path = argv[1];
    raw = slurp();
    if (raw == NULL) {
        return 1;
    }
    line = raw;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        char saved = 0;
        dlmm_cap_rec_t rec;
        if (nl) {
            saved = *nl;
            *nl = 0;
        }
        if (line[0] == '{') {
            if (fill_rec(line, &rec) != 0) {
                fprintf(stderr, "bad line\n");
                return 1;
            }
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                recs = realloc(recs, cap * sizeof(*recs));
            }
            recs[n++] = rec;
        }
        if (!nl) {
            break;
        }
        *nl = saved;
        line = nl + 1;
    }
    free(raw);
    if (dlmm_cap_save(out_path, recs, n) != 0) {
        fprintf(stderr, "save failed\n");
        return 1;
    }
    fprintf(stderr, "wrote %u records to %s\n", n, out_path);
    free(recs);
    (void)f;
    return 0;
}
