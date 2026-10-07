#include "shred/txframe.h"
#include "shred/entry.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(c, m)                       \
    do {                                  \
        if (!(c)) {                       \
            fprintf(stderr, "FAIL  %s\n", (m)); \
            g_fail++;                     \
        } else {                          \
            printf("ok    %s\n", (m));    \
        }                                 \
    } while (0)

static void
put_sv(uint8_t *p, uint32_t *off, uint32_t v)
{
    do {
        uint8_t b = (uint8_t)(v & 0x7fu);

        v >>= 7;
        if (v != 0) {
            b |= 0x80u;
        }
        p[(*off)++] = b;
    } while (v != 0);
}

int
main(void)
{
    uint8_t legacy[256];
    uint8_t v0[256];
    uint8_t badver[8];
    txframe_t tf;
    uint32_t off;
    int rc;

    memset(legacy, 0, sizeof(legacy));
    off = 0;
    put_sv(legacy, &off, 1);
    legacy[off] = 0x11;
    off += 64;
    legacy[off++] = 1;
    legacy[off++] = 0;
    legacy[off++] = 0;
    put_sv(legacy, &off, 1);
    memset(legacy + off, 0x22, 32);
    off += 32;
    memset(legacy + off, 0x33, 32);
    off += 32;
    put_sv(legacy, &off, 0);
    rc = txframe_parse(legacy, off, &tf);
    CHECK(rc == TXF_OK, "legacy ok");
    CHECK(tf.version == TXF_ENC_LEGACY, "legacy enc");
    CHECK(tf.tx_len == off, "legacy len");
    CHECK(tf.sig[0] == 0x11, "legacy sig0");

    memset(v0, 0, sizeof(v0));
    off = 0;
    put_sv(v0, &off, 1);
    v0[off] = 0x44;
    off += 64;
    v0[off++] = 0x80;
    v0[off++] = 1;
    v0[off++] = 0;
    v0[off++] = 0;
    put_sv(v0, &off, 1);
    memset(v0 + off, 0x55, 32);
    off += 32;
    memset(v0 + off, 0x66, 32);
    off += 32;
    put_sv(v0, &off, 0);
    put_sv(v0, &off, 0);
    rc = txframe_parse(v0, off, &tf);
    CHECK(rc == TXF_OK, "v0 ok");
    CHECK(tf.version == TXF_ENC_V0, "v0 enc");
    CHECK(tf.message_off == 65, "v0 msg off");

    memset(badver, 0, sizeof(badver));
    badver[0] = 0x82;
    CHECK(txframe_parse(badver, 8, &tf) == TXF_UNSUPPORTED_VERSION,
          "0x82 not shortvec");
    CHECK(txframe_parse((const uint8_t *)"\x81", 1, &tf) == TXF_NEED_MORE_BYTES,
          "v1 need more");

    {
        uint8_t vec[16];
        uint32_t cons = 0;
        int erc = 0;

        memset(vec, 0, sizeof(vec));
        CHECK(shred_entries_parse(vec, 8, NULL, NULL, &cons, &erc) == ENT_OK,
              "empty vec");
    }

    if (g_fail != 0) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("txframe ok\n");
    return 0;
}
