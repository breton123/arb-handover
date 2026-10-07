#include "classify.h"
#include "frame.h"
#include "trigger.h"
#include "watch.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", m); g_fail++; } } while (0)

static const uint8_t PAYER[32] = { 1, };
static const uint8_t POOL[32] = { 2, };
static const uint8_t HASH[32] = { 3, };
static const uint8_t SIG[64] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21,
    0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
    0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31
};
static const uint8_t PUMP_SELL[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};

static uint16_t
put_u64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
    return 8;
}

static uint32_t
build_legacy(uint8_t *b)
{
    uint8_t *p = b;
    *p++ = 1;
    memcpy(p, SIG, 64);
    p += 64;
    *p++ = 1;
    *p++ = 0;
    *p++ = 0;
    *p++ = 3;
    memcpy(p, PAYER, 32);
    p += 32;
    memcpy(p, PROG_PUMP, 32);
    p += 32;
    memcpy(p, POOL, 32);
    p += 32;
    memcpy(p, HASH, 32);
    p += 32;
    *p++ = 1;
    *p++ = 1;
    *p++ = 2;
    *p++ = 0;
    *p++ = 2;
    *p++ = 24;
    memcpy(p, PUMP_SELL, 8);
    p += 8;
    p += put_u64(p, 10000);
    p += put_u64(p, 1);
    return (uint32_t)(p - b);
}

static uint32_t
build_v0(uint8_t *b)
{
    uint8_t *p = b;
    *p++ = 1;
    memcpy(p, SIG, 64);
    p += 64;
    *p++ = 0x80;
    *p++ = 1;
    *p++ = 0;
    *p++ = 0;
    *p++ = 3;
    memcpy(p, PAYER, 32);
    p += 32;
    memcpy(p, PROG_PUMP, 32);
    p += 32;
    memcpy(p, POOL, 32);
    p += 32;
    memcpy(p, HASH, 32);
    p += 32;
    *p++ = 1;
    *p++ = 1;
    *p++ = 2;
    *p++ = 0;
    *p++ = 2;
    *p++ = 24;
    memcpy(p, PUMP_SELL, 8);
    p += 8;
    p += put_u64(p, 10000);
    p += put_u64(p, 1);
    *p++ = 0;
    return (uint32_t)(p - b);
}

static uint32_t
build_v1(uint8_t *b, uint16_t extra)
{
    uint8_t *p = b;
    uint16_t dlen = (uint16_t)(24u + extra);
    uint32_t i;
    *p++ = TX_V1_PREFIX;
    *p++ = 1;
    *p++ = 0;
    *p++ = 1;
    *p++ = 0x0c;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    memcpy(p, HASH, 32);
    p += 32;
    *p++ = 1;
    *p++ = 3;
    memcpy(p, PAYER, 32);
    p += 32;
    memcpy(p, POOL, 32);
    p += 32;
    memcpy(p, PROG_PUMP, 32);
    p += 32;
    memset(p, 0x10, 4);
    p += 4;
    memset(p, 0x20, 4);
    p += 4;
    *p++ = 2;
    *p++ = 2;
    *p++ = (uint8_t)(dlen & 0xffu);
    *p++ = (uint8_t)(dlen >> 8);
    *p++ = 0;
    *p++ = 1;
    memcpy(p, PUMP_SELL, 8);
    p += 8;
    p += put_u64(p, 10000);
    p += put_u64(p, 1);
    for (i = 0; i < extra; i++) {
        *p++ = 0x5a;
    }
    memcpy(p, SIG, 64);
    p += 64;
    return (uint32_t)(p - b);
}

int
main(int argc, char **argv)
{
    if (argc == 2) {
        FILE *f = fopen(argv[1], "rb");
        uint8_t buf[TX_V1_MAX];
        size_t n;
        frame_hit_t h;
        int rc;
        if (f == NULL) {
            fprintf(stderr, "open %s\n", argv[1]);
            return 1;
        }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        rc = frame_try_any(buf, (uint32_t)n, 0, &h);
        printf("file %s n=%u rc=%d ver=%u nsig=%u nkeys=%u ninstr=%u end=%u dex=%u\n",
               argv[1], (unsigned)n, rc, (unsigned)h.versioned,
               (unsigned)h.nsig, (unsigned)h.nkeys, (unsigned)h.ninstr,
               (unsigned)h.tx_end, (unsigned)h.have_dex);
        return rc == 0 ? 0 : 1;
    }
    uint8_t tx[4096];
    uint8_t fat[4096];
    uint8_t buf[4096];
    uint32_t n, fatn;
    frame_hit_t h;
    int rc;

    n = build_legacy(tx);
    rc = frame_try_any(tx, n, 0, &h);
    CHECK(rc == 0 && h.klass == FRAME_FRAMED, "legacy FRAME");
    CHECK(h.versioned == FRAME_VER_LEGACY, "legacy ver");
    CHECK(frame_around_sig(tx, n, SIG, &h) == FRAME_FRAMED, "legacy around");

    n = build_v0(tx);
    rc = frame_try_any(tx, n, 0, &h);
    CHECK(rc == 0 && h.klass == FRAME_FRAMED, "v0 FRAME");
    CHECK(h.versioned == FRAME_VER_V0, "v0 ver");
    CHECK(frame_try_at(tx, n, 0, &h) == 0, "v0 fast-path DEX");
    CHECK(frame_around_sig(tx, n, SIG, &h) == FRAME_FRAMED, "v0 around");

    n = build_v1(tx, 0);
    rc = frame_try_any(tx, n, 0, &h);
    CHECK(rc == 0 && h.klass == FRAME_FRAMED, "v1 complete FRAME");
    CHECK(h.versioned == FRAME_VER_V1 && h.nsig == 1 && h.nkeys == 3
          && h.ninstr == 1, "v1 counts");
    CHECK(h.have_dex == 1, "v1 dex");
    CHECK(h.tx_end == n, "v1 end");
    CHECK(frame_try_at(tx, n, 0, &h) == 0, "v1 require_dex");
    CHECK(frame_around_sig(tx, n, SIG, &h) == FRAME_FRAMED, "v1 around tail");
    CHECK(h.versioned == FRAME_VER_V1, "v1 around ver");

    memset(buf, 0xa5, 16);
    memcpy(buf + 16, tx, n);
    CHECK(frame_around_sig(buf, 16 + n, SIG, &h) == FRAME_FRAMED,
          "v1 around padded");
    CHECK(h.tx_off == 16 && h.tx_end == 16 + n, "v1 padded offs");

    rc = frame_try_any(tx, n / 2, 0, &h);
    CHECK(rc == 1, "v1 fragmented INCOMPLETE");
    rc = frame_try_any(tx, n, 0, &h);
    CHECK(rc == 0, "v1 fragment then FRAME");

    rc = frame_try_any(tx, n - 8, 0, &h);
    CHECK(rc == 1, "truncated v1 INCOMPLETE");
    CHECK(frame_around_sig(tx, n - 8, SIG, &h) == FRAME_INCOMPLETE,
          "truncated around INCOMPLETE");

    memcpy(buf, tx, n);
    buf[4] = 0x20;
    rc = frame_try_any(buf, n, 0, &h);
    CHECK(rc == -1, "unknown config bit INVALID");

    memcpy(buf, tx, n);
    buf[1] = 0;
    rc = frame_try_any(buf, n, 0, &h);
    CHECK(rc == -1, "nsig 0 INVALID");

    memcpy(buf, tx, n);
    memcpy(buf + 42 + 32, PAYER, 32);
    rc = frame_try_any(buf, n, 0, &h);
    CHECK(rc == -1, "duplicate address INVALID");

    memcpy(buf, tx, n);
    buf[0] = 0x82;
    rc = frame_try_any(buf, n, 0, &h);
    CHECK(rc == -1, "unknown version INVALID");

    fatn = build_v1(fat, 1832);
    CHECK(fatn >= 2070 && fatn <= 2080 && fatn < TX_V1_MAX, "fat 2072-class");
    rc = frame_try_any(fat, fatn, 0, &h);
    CHECK(rc == 0 && h.versioned == FRAME_VER_V1, "2072-class v1 FRAME");
    CHECK(frame_around_sig(fat, fatn, SIG, &h) == FRAME_FRAMED, "fat around");

    {
        alt_cache_t alts;
        watch_idx_t watch;
        trigger_hit_t hit;
        trigger_metrics_t met;
        n = build_v1(tx, 0);
        CHECK(alt_cache_init(&alts) == 0 && watch_init(&watch) == 0, "idx");
        CHECK(watch_put(&watch, POOL, 9, PROTO_PUMP, WATCH_ROLE_POOL) == 0,
              "watch");
        memset(&met, 0, sizeof(met));
        rc = trigger_eval(tx, n, 0, &alts, &watch, &hit, &met);
        CHECK(rc == TRIG_EXACT && hit.exact.amount_in == 10000, "v1 IX_EXACT");
        CHECK(hit.relevant && hit.n_watch >= 1 && hit.hit_pool[0] == 9,
              "v1 watched");
        CHECK(hit.versioned == FRAME_VER_V1 && hit.nlut == 0, "v1 no ALT");
        alt_cache_free(&alts);
        watch_free(&watch);
    }

    printf("FRAME-V1  %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
