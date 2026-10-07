/*
 * PAPER-004 — replay sync.bin against INIT liveuniv.
 * Classify every known-pool decide. Emit predicted S' for the 34.
 * Does not send. Does not touch feed_live.
 */
#include "dlmm_cache.h"
#include "hot.h"
#include "live.h"
#include "proto.h"
#include "swapix.h"
#include "syncrec.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MINT_SOL in proto.h */

static void
hex32(const uint8_t *p, char *out)
{
    static const char *h = "0123456789abcdef";
    uint32_t i;
    for (i = 0; i < 32; i++) {
        out[i * 2] = h[p[i] >> 4];
        out[i * 2 + 1] = h[p[i] & 15];
    }
    out[64] = 0;
}

static void
hex64(const uint8_t *p, char *out)
{
    static const char *h = "0123456789abcdef";
    uint32_t i;
    for (i = 0; i < 64; i++) {
        out[i * 2] = h[p[i] >> 4];
        out[i * 2 + 1] = h[p[i] & 15];
    }
    out[128] = 0;
}

static int
mint_is_wsol(const uint8_t *m)
{
    return memcmp(m, MINT_SOL, 32) == 0;
}

static int
read_rec(FILE *f, uint8_t *kind, uint64_t *ts, uint8_t *body, uint32_t *blen)
{
    uint32_t ln;
    uint8_t hdr[16];

    if (fread(hdr, 1, 16, f) != 16) {
        return 1;
    }
    memcpy(&ln, hdr, 4);
    *kind = hdr[4];
    memcpy(ts, hdr + 8, 8);
    if (ln < 12 || ln - 12u > 8192u) {
        return -1;
    }
    *blen = ln - 12u;
    if (fread(body, 1, *blen, f) != *blen) {
        return -1;
    }
    return 0;
}

static void
fill_ix(swapix_t *n, uint8_t proto, uint8_t dir, uint64_t ain, uint64_t mino,
        const uint8_t sig[64], const uint8_t pool[32])
{
    memset(n, 0, sizeof(*n));
    n->protocol = proto;
    n->direction = dir;
    n->amount_in = ain;
    n->min_out = mino;
    n->have_sig = 1;
    memcpy(n->sig, sig, 64);
    memcpy(n->pool, pool, 32);
}

static const char *
classify(live_univ_t *u, uint32_t idx, const swapix_t *n,
         hot_decision_t *dec, dlmm_pool_hot_t *spec_out, pump_state_t *pump_out)
{
    hot_trigger_t trig;
    uint8_t mp;
    const dlmm_pool_hot_t *h;
    int32_t off;
    uint32_t i;

    memset(dec, 0, sizeof(*dec));
    if (spec_out) {
        memset(spec_out, 0, sizeof(*spec_out));
    }
    if (pump_out) {
        memset(pump_out, 0, sizeof(*pump_out));
    }
    if (idx >= u->n) {
        return "POOL_OOB";
    }
    mp = u->meta[idx].protocol;
    if (mp != PROTO_DLMM && mp != PROTO_PUMP) {
        return "VENUE_NOT_ROUTE0";
    }
    if (n->protocol != mp) {
        return "PROTO_MISMATCH";
    }
    if (swapix_to_trigger(n, &trig) != 0) {
        return "TRIGGER_BAD";
    }
    if (mp == PROTO_DLMM) {
        dlmm_pool_hot_t spec;
        dlmm_apply_result_t res;
        h = dlmm_cache_get(&u->dlmm, idx);
        if (h == NULL) {
            return "DLMM_NO_CACHE";
        }
        off = h->active_id - h->window_center;
        if (off < -(int32_t)DLMM_CACHE_K || off > (int32_t)DLMM_CACHE_K) {
            return "DLMM_ACTIVE_OFF_WINDOW";
        }
        if (dlmm_cache_predict(&u->dlmm, idx, &trig.dlmm, &spec, &res) != 0) {
            return "DLMM_MISSING_BIN";
        }
        if (spec_out) {
            *spec_out = spec;
        }
        for (i = 0; i < u->n; i++) {
            if (i != idx && u->meta[i].protocol == PROTO_PUMP && u->pump_live[i]
                && ((memcmp(u->meta[i].mint_x, u->meta[idx].mint_x, 32) == 0
                     && memcmp(u->meta[i].mint_y, u->meta[idx].mint_y, 32) == 0)
                    || (memcmp(u->meta[i].mint_x, u->meta[idx].mint_y, 32) == 0
                        && memcmp(u->meta[i].mint_y, u->meta[idx].mint_x, 32) == 0))) {
                break;
            }
        }
        if (i >= u->n) {
            return "NO_PUMP_PARTNER";
        }
    } else {
        pump_state_t after;
        pump_swap_result_t res;
        if (!u->pump_live[idx]) {
            return "PUMP_NOT_LIVE";
        }
        if (pump_apply_swap(&u->pump[idx], &trig.pump, &after, &res) != 0) {
            return "PUMP_APPLY_FAIL";
        }
        if (pump_out) {
            *pump_out = after;
        }
        h = NULL;
        for (i = 0; i < u->n; i++) {
            if (i != idx && u->meta[i].protocol == PROTO_DLMM
                && dlmm_cache_get(&u->dlmm, i) != NULL
                && ((memcmp(u->meta[i].mint_x, u->meta[idx].mint_x, 32) == 0
                     && memcmp(u->meta[i].mint_y, u->meta[idx].mint_y, 32) == 0)
                    || (memcmp(u->meta[i].mint_x, u->meta[idx].mint_y, 32) == 0
                        && memcmp(u->meta[i].mint_y, u->meta[idx].mint_x, 32) == 0))) {
                h = dlmm_cache_get(&u->dlmm, i);
                break;
            }
        }
        if (h == NULL) {
            return "NO_DLMM_PARTNER";
        }
    }
    if (hot_decide(u, idx, &trig, dec) != 0) {
        return "CYCLE_FAIL";
    }
    return dec->opp.valid ? "OK" : "NO_OPP";
}

int
main(int argc, char **argv)
{
    live_univ_t u;
    FILE *sf;
    FILE *out;
    uint8_t magic[16];
    uint8_t body[8192];
    const char *univ;
    const char *sync;
    const char *json_path;
    uint32_t n_dec = 0, n_mut = 0, n_ok = 0, n_opp = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s liveuniv.bin sync.bin out.jsonl\n", argv[0]);
        return 1;
    }
    univ = argv[1];
    sync = argv[2];
    json_path = argv[3];
    memset(&u, 0, sizeof(u));
    if (live_univ_init(&u) != 0 || live_univ_load(&u, univ) != 0) {
        fprintf(stderr, "univ load failed\n");
        return 1;
    }
    sf = fopen(sync, "rb");
    out = fopen(json_path, "w");
    if (sf == NULL || out == NULL || fread(magic, 1, 16, sf) != 16) {
        fprintf(stderr, "sync open failed\n");
        return 1;
    }
    for (;;) {
        uint8_t kind;
        uint64_t ts;
        uint32_t blen;
        int rc = read_rec(sf, &kind, &ts, body, &blen);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fprintf(stderr, "sync truncated\n");
            break;
        }
        if (kind == SYN_KIND_DEC && blen >= 140) {
            swapix_t n;
            hot_decision_t dec;
            hot_trigger_t trig;
            dlmm_pool_hot_t spec;
            pump_state_t ps;
            uint32_t idx;
            uint8_t proto, dir, ov, od;
            uint64_t ain, mino, ver, oa, oo, gp;
            uint32_t rid;
            char sig_h[129], pk_h[65], vx[65], vy[65], mx[65], my[65];
            const char *why;
            const char *kind_s;

            memcpy(&idx, body + 64, 4);
            proto = body[68];
            dir = body[69];
            memcpy(&ain, body + 70, 8);
            memcpy(&mino, body + 78, 8);
            memcpy(&ver, body + 86, 8);
            memcpy(&rid, body + 110, 4);
            memcpy(&oa, body + 114, 8);
            memcpy(&oo, body + 122, 8);
            memcpy(&gp, body + 130, 8);
            od = body[138];
            ov = body[139];
            fill_ix(&n, proto, dir, ain, mino, body, idx < u.n ? u.meta[idx].pubkey : body);
            why = classify(&u, idx, &n, &dec, &spec, &ps);
            hex64(body, sig_h);
            if (idx < u.n) {
                hex32(u.meta[idx].pubkey, pk_h);
                hex32(u.meta[idx].vault_x, vx);
                hex32(u.meta[idx].vault_y, vy);
                hex32(u.meta[idx].mint_x, mx);
                hex32(u.meta[idx].mint_y, my);
                kind_s = u.meta[idx].protocol == PROTO_DLMM ? "dlmm"
                       : u.meta[idx].protocol == PROTO_PUMP ? "pump"
                       : u.meta[idx].protocol == PROTO_CLMM ? "clmm"
                       : u.meta[idx].protocol == PROTO_CPMM ? "cpmm"
                       : u.meta[idx].protocol == PROTO_DAMM ? "damm"
                       : u.meta[idx].protocol == PROTO_ORCA ? "orca" : "other";
            } else {
                pk_h[0] = 0;
                vx[0] = vy[0] = mx[0] = my[0] = 0;
                kind_s = "oob";
            }
            fprintf(out,
                    "{\"kind\":\"dec\",\"ts_ns\":%" PRIu64
                    ",\"sig\":\"%s\",\"pool_idx\":%u,\"pool\":\"%s\""
                    ",\"pool_kind\":\"%s\",\"n_proto\":%u,\"n_dir\":%u"
                    ",\"n_ain\":%" PRIu64 ",\"n_min_out\":%" PRIu64
                    ",\"state_version\":%" PRIu64
                    ",\"opp_valid\":%u,\"opp_dir\":%u,\"opp_ain\":%" PRIu64
                    ",\"opp_aout\":%" PRIu64 ",\"opp_gp\":%" PRIu64
                    ",\"why\":\"%s\",\"sol_x\":%u,\"sol_y\":%u"
                    ",\"mint_x\":\"%s\",\"mint_y\":\"%s\""
                    ",\"vault_x\":\"%s\",\"vault_y\":\"%s\"",
                    ts, sig_h, idx, pk_h, kind_s, proto, dir, ain, mino, ver,
                    ov, od, oa, oo, gp, why,
                    idx < u.n ? mint_is_wsol(u.meta[idx].mint_x) : 0,
                    idx < u.n ? mint_is_wsol(u.meta[idx].mint_y) : 0,
                    mx, my, vx, vy);
            if (u.meta[idx].protocol == PROTO_DLMM && spec.occupied) {
                fprintf(out,
                        ",\"sprime\":{\"proto\":\"dlmm\",\"active_id\":%d"
                        ",\"reserve_x\":%" PRIu64 ",\"reserve_y\":%" PRIu64
                        ",\"window_center\":%d}",
                        spec.active_id, spec.reserve_x, spec.reserve_y,
                        spec.window_center);
            } else if (u.meta[idx].protocol == PROTO_PUMP && ps.reserve_base) {
                fprintf(out,
                        ",\"sprime\":{\"proto\":\"pump\",\"reserve_base\":%" PRIu64
                        ",\"reserve_quote\":%" PRIu64 "}",
                        ps.reserve_base, ps.reserve_quote);
            }
            fprintf(out, "}\n");
            n_dec++;
            if (ov) {
                n_opp++;
            }
            if (swapix_to_trigger(&n, &trig) == 0 && hot_commit(&u, idx, &trig) == 0) {
                n_ok++;
            }
        } else if (kind == SYN_KIND_MUT && blen >= 102) {
            /* commits already applied above when DEC replayed successfully */
            n_mut++;
            (void)ts;
        }
    }
    fclose(sf);
    fclose(out);
    fprintf(stderr,
            "PAPER-004 replay  dec=%u mut=%u commit_ok=%u opp=%u ver_now=%" PRIu64 "\n",
            n_dec, n_mut, n_ok, n_opp, u.state_version);
    live_univ_free(&u);
    return 0;
}
