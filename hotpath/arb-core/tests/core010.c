#include "classify.h"
#include "frame.h"

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

static uint16_t
build_tx(uint8_t *b)
{
    uint8_t *p = b;
    /* nsig=1, sig, legacy header, 3 keys, 1 pump ix */
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
    memset(p, 0x7a, 24);
    p += 24;
    return (uint16_t)(p - b);
}

int
main(void)
{
    uint8_t tx[512];
    uint8_t buf[512];
    uint16_t n = build_tx(tx);
    frame_hit_t h;
    int k;

    k = frame_around_sig(tx, n, SIG, &h);
    CHECK(k == FRAME_FRAMED, "full tx framed");
    CHECK(h.nsig == 1 && h.nkeys == 3 && h.ninstr == 1, "counts");

    k = frame_around_sig(tx, (uint16_t)(n - 8), SIG, &h);
    CHECK(k == FRAME_INCOMPLETE, "truncated");

    memcpy(buf, tx, n);
    buf[0] = 0;
    k = frame_around_sig(buf, n, SIG, &h);
    CHECK(k == FRAME_INVALID, "nsig 0");

    memset(buf, 0, sizeof(buf));
    buf[0] = 1;
    buf[1] = 0xae;
    memcpy(buf + 2, SIG, 64);
    k = frame_around_sig(buf, 2 + 64, SIG, &h);
    CHECK(k == FRAME_INVALID || k == FRAME_INCOMPLETE, "01 ae + sig not framed");
    /* with extra tail still not signatures[0] */
    memset(buf + 66, 0x5a, 40);
    k = frame_around_sig(buf, 106, SIG, &h);
    CHECK(k == FRAME_INVALID, "01 ae + sig + tail invalid");

    printf("CORE-010  %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
