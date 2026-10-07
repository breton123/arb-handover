#include "alt_cache.h"
#include "classify.h"
#include "classify_n.h"
#include "frame.h"
#include "trigger.h"
#include "watch.h"

#include <stdio.h>
#include <string.h>

static const uint8_t PUMP_SELL[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};

static uint32_t
put_u64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
    return 8;
}

static uint32_t
build_v0_alt_pump(uint8_t *tx, const uint8_t pump[32], const uint8_t pool[32],
                  const uint8_t altpk[32])
{
    uint32_t o = 0;
    uint64_t amt = 10000, mino = 1;
    tx[o++] = 1;
    memset(tx + o, 0x11, 64);
    o += 64;
    tx[o++] = 0x80;
    tx[o++] = 1;
    tx[o++] = 0;
    tx[o++] = 0;
    tx[o++] = 2;
    memset(tx + o, 0xaa, 32);
    o += 32;
    memset(tx + o, 0xbb, 32);
    o += 32;
    memset(tx + o, 0xcc, 32);
    o += 32;
    tx[o++] = 1;
    tx[o++] = 2;
    tx[o++] = 1;
    tx[o++] = 3;
    tx[o++] = 24;
    memcpy(tx + o, PUMP_SELL, 8);
    o += 8;
    o += put_u64(tx + o, amt);
    o += put_u64(tx + o, mino);
    tx[o++] = 1;
    memcpy(tx + o, altpk, 32);
    o += 32;
    tx[o++] = 2;
    tx[o++] = 0;
    tx[o++] = 1;
    tx[o++] = 0;
    (void)pump;
    (void)pool;
    return o;
}

static uint32_t
build_v0_multi_alt_pump(uint8_t *tx, const uint8_t alt1[32],
                        const uint8_t alt2[32])
{
    uint32_t o = 0;
    uint64_t amt = 10000, mino = 1;
    tx[o++] = 1;
    memset(tx + o, 0x12, 64);
    o += 64;
    tx[o++] = 0x80;
    tx[o++] = 1;
    tx[o++] = 0;
    tx[o++] = 0;
    tx[o++] = 2;
    memset(tx + o, 0xaa, 32);
    o += 32;
    memset(tx + o, 0xbb, 32);
    o += 32;
    memset(tx + o, 0xcc, 32);
    o += 32;
    tx[o++] = 1;
    tx[o++] = 6; /* static 2 + writable 2 + readonly 2 */
    tx[o++] = 1;
    tx[o++] = 3; /* second table's writable pool in canonical key order */
    tx[o++] = 24;
    memcpy(tx + o, PUMP_SELL, 8);
    o += 8;
    o += put_u64(tx + o, amt);
    o += put_u64(tx + o, mino);
    tx[o++] = 2;
    memcpy(tx + o, alt1, 32);
    o += 32;
    tx[o++] = 1;
    tx[o++] = 0; /* writable pool1 */
    tx[o++] = 1;
    tx[o++] = 1; /* readonly filler1 */
    memcpy(tx + o, alt2, 32);
    o += 32;
    tx[o++] = 1;
    tx[o++] = 0; /* writable pool2 */
    tx[o++] = 2;
    tx[o++] = 1; /* readonly filler2 */
    tx[o++] = 2; /* readonly Pump program */
    return o;
}

static uint32_t
build_unknown_router(uint8_t *tx, const uint8_t pool[32])
{
    uint32_t o = 0;
    tx[o++] = 1;
    memset(tx + o, 0x22, 64);
    o += 64;
    tx[o++] = 1;
    tx[o++] = 0;
    tx[o++] = 0;
    tx[o++] = 2;
    memset(tx + o, 0xaa, 32);
    o += 32;
    memcpy(tx + o, pool, 32);
    o += 32;
    memset(tx + o, 0xcc, 32);
    o += 32;
    tx[o++] = 1;
    tx[o++] = 0;
    tx[o++] = 1;
    tx[o++] = 1;
    tx[o++] = 24;
    memset(tx + o, 0x99, 24);
    o += 24;
    return o;
}

static uint32_t
build_static_pump(uint8_t *tx, const uint8_t pool[32], const uint8_t alt[32], int v0)
{
    uint32_t o = 0;
    tx[o++] = 1; memset(tx + o, 0x14, 64); o += 64;
    if (v0) tx[o++] = 0x80;
    tx[o++] = 1; tx[o++] = 0; tx[o++] = 1; tx[o++] = 3;
    memset(tx + o, 0xaa, 32); o += 32;
    memcpy(tx + o, pool, 32); o += 32;
    memcpy(tx + o, PROG_PUMP, 32); o += 32;
    memset(tx + o, 0xcc, 32); o += 32;
    tx[o++] = 1; tx[o++] = 2; tx[o++] = 1; tx[o++] = 1; tx[o++] = 24;
    memcpy(tx + o, PUMP_SELL, 8); o += 8;
    o += put_u64(tx + o, 10000); o += put_u64(tx + o, 1);
    if (v0) {
        tx[o++] = 1; memcpy(tx + o, alt, 32); o += 32;
        tx[o++] = 0; tx[o++] = 1; tx[o++] = 0;
    }
    return o;
}

int
main(void)
{
    uint8_t tx[512];
    uint8_t altpk[32];
    uint8_t pool[32];
    uint8_t alt_addrs[64];
    uint32_t n;
    frame_hit_t fh;
    alt_cache_t alts;
    watch_idx_t watch;
    trigger_hit_t hit;
    trigger_metrics_t met;
    int rc;
    alt_meta_t meta = {.observed_slot = 100, .last_extended_slot = 99,
                       .deactivation_slot = UINT64_MAX};
    alt_bank_t bank = {.slot = 100, .slot_hashes_complete = 1};
    memset(meta.observed_bank_hash, 0x10, 32);
    memset(bank.bank_hash, 0x10, 32);

    memset(altpk, 0xdd, 32);
    memset(pool, 0xee, 32);
    memcpy(alt_addrs, PROG_PUMP, 32);
    memcpy(alt_addrs + 32, pool, 32);

    n = build_v0_alt_pump(tx, PROG_PUMP, pool, altpk);
    if (frame_try_at(tx, n, 0, &fh) == 0) {
        fprintf(stderr, "fast-path should reject ALT-only DEX\n");
        return 1;
    }
    if (frame_try_any(tx, n, 0, &fh) != 0 || fh.klass != FRAME_FRAMED) {
        fprintf(stderr, "generic frame failed\n");
        return 1;
    }
    if (classify_n(tx, n, 2) != REL_N_NONE) {
        fprintf(stderr, "classify_n should miss ALT-only DEX\n");
        return 1;
    }
    if (alt_cache_init(&alts) != 0 || watch_init(&watch) != 0) {
        return 1;
    }
    if (alt_cache_put_meta(&alts, altpk, alt_addrs, 2, &meta, 7) != 0
        || watch_put(&watch, pool, 7, PROTO_PUMP, WATCH_ROLE_POOL) != 0) {
        return 1;
    }
    memset(&met, 0, sizeof(met));
    rc = trigger_eval(tx, n, 0, &alts, &watch, &hit, &met);
    if (rc != TRIG_ALT_UNCERTAIN || hit.exact.amount_in != 0 || hit.alt_status != ALT_BANK_UNPROVEN) {
        fprintf(stderr, "missing bank certificate did not fail closed\n");
        return 1;
    }
    {
        alt_bank_t shred = {.slot = 100};
        rc = trigger_eval_at(tx, n, 0, &alts, &watch, &shred, &hit, &met);
        if (rc != TRIG_EXACT || hit.exact.amount_in != 10000 || !hit.relevant
            || hit.alt_status != ALT_BANK_UNPROVEN) {
            fprintf(stderr, "shred-slot live resolve failed klass=%u amt=%llu st=%u\n",
                    (unsigned)rc, (unsigned long long)hit.exact.amount_in,
                    (unsigned)hit.alt_status);
            return 1;
        }
    }
    rc = trigger_eval_at(tx, n, 0, &alts, &watch, &bank, &hit, &met);
    if (rc != TRIG_EXACT || hit.exact.protocol != PROTO_PUMP
        || hit.exact.amount_in != 10000 || !hit.relevant) {
        fprintf(stderr, "exact ALT pump failed klass=%u amt=%llu\n",
                (unsigned)rc, (unsigned long long)hit.exact.amount_in);
        return 1;
    }
    if (hit.n_watch < 1 || hit.hit_pool[0] != 7) {
        fprintf(stderr, "watch miss\n");
        return 1;
    }

    {
        uint8_t alt1[32], alt2[32], pool1[32], pool2[32];
        uint8_t addrs1[64], addrs2[96];
        memset(alt1, 0xd1, sizeof(alt1));
        memset(alt2, 0xd2, sizeof(alt2));
        memset(pool1, 0xe1, sizeof(pool1));
        memset(pool2, 0xe2, sizeof(pool2));
        memcpy(addrs1, pool1, 32);
        memset(addrs1 + 32, 0xf1, 32);
        memcpy(addrs2, pool2, 32);
        memset(addrs2 + 32, 0xf2, 32);
        memcpy(addrs2 + 64, PROG_PUMP, 32);
        if (alt_cache_put_meta(&alts, alt1, addrs1, 2, &meta, 7) != 0
            || alt_cache_put_meta(&alts, alt2, addrs2, 3, &meta, 7) != 0
            || watch_put(&watch, pool2, 8, PROTO_PUMP, WATCH_ROLE_POOL) != 0) {
            return 1;
        }
        n = build_v0_multi_alt_pump(tx, alt1, alt2);
        rc = trigger_eval_at(tx, n, 0, &alts, &watch, &bank, &hit, &met);
        if (rc != TRIG_EXACT || !hit.relevant || hit.n_loaded != 7
            || hit.hit_pool[0] != 8 || hit.exact.amount_in != 10000) {
            fprintf(stderr, "multi-LUT canonical ordering failed klass=%u loaded=%u\n",
                    (unsigned)rc, (unsigned)hit.n_loaded);
            return 1;
        }
    }

    n = build_unknown_router(tx, pool);
    rc = trigger_eval(tx, n, 0, &alts, &watch, &hit, &met);
    if (rc != TRIG_RELEVANT_UNKNOWN || hit.exact.amount_in != 0) {
        fprintf(stderr, "unknown must fail closed klass=%u amt=%llu\n",
                (unsigned)rc, (unsigned long long)hit.exact.amount_in);
        return 1;
    }

    {
        uint8_t extra_pk[32], extra_addr[32], signature[64];
        swapix_t fast;
        memset(extra_pk, 0xa5, 32); memset(extra_addr, 0xf5, 32);
        memset(signature, 0x14, 64);
        if (alt_cache_put_meta(&alts, extra_pk, extra_addr, 1, &meta, 7)) return 1;
        n = build_static_pump(tx, pool, extra_pk, 1);
        if (swapix_from_tx(tx, (uint16_t)n, 0, &fast)) {
            fprintf(stderr, "static v0 bypass fixture was not discovered\n"); return 1;
        }
        if (trigger_admit_at(tx, n, signature, &alts, &watch, NULL, &hit) != TRIG_ALT_UNCERTAIN
            || hit.exact.amount_in != 0) {
            fprintf(stderr, "static path bypassed bank admission\n"); return 1;
        }
        if (trigger_admit_at(tx, n, signature, &alts, &watch, &bank, &hit) != TRIG_EXACT) return 1;
        signature[0] ^= 1;
        if (trigger_admit_at(tx, n, signature, &alts, &watch, &bank, &hit) != TRIG_INVALID) return 1;
        signature[0] ^= 1;
        tx[n] = 0;
        if (trigger_admit_at(tx, n + 1, signature, &alts, &watch, &bank, &hit) != TRIG_INVALID) return 1;
        n = build_static_pump(tx, pool, extra_pk, 0);
        if (trigger_admit_at(tx, n, signature, &alts, &watch, NULL, &hit) != TRIG_EXACT) return 1;
    }

    {
        alt_cache_t empty;
        trigger_hit_t miss;
        n = build_v0_alt_pump(tx, PROG_PUMP, pool, altpk);
        if (alt_cache_init(&empty) != 0) {
            return 1;
        }
        rc = trigger_eval(tx, n, 0, &empty, &watch, &miss, &met);
        if (rc != TRIG_ALT_MISS || miss.exact.amount_in != 0) {
            fprintf(stderr, "alt miss expected klass=%u\n", (unsigned)rc);
            return 1;
        }
        alt_cache_free(&empty);
    }

    {
        const char *tmp = "/tmp/t011_alt.bin";
        alt_cache_t reload;
        const uint8_t *addrs = NULL;
        uint16_t an = 0;
        if (alt_cache_save(&alts, tmp) != 0 || alt_cache_init(&reload) != 0
            || alt_cache_load(&reload, tmp) != 0
            || alt_cache_get(&reload, altpk, &addrs, &an) != 0 || an != 2) {
            fprintf(stderr, "alt persist failed\n");
            return 1;
        }
        alt_cache_free(&reload);
    }

    alt_cache_free(&alts);
    watch_free(&watch);
    printf("TRIGGER011 unit  generic_frame=1 alt_exact=1 multi_alt_order=1 unknown_closed=1 "
           "classify_n_unregressed=1 alt_persist=1\n");
    return 0;
}
