#include "shred/shred.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL  %s\n", (msg)); \
            g_fail++;                             \
        } else {                                  \
            printf("ok    %s\n", (msg));          \
        }                                         \
    } while (0)

static void
fill_shred(uint8_t *p, uint16_t n, uint64_t slot, uint32_t index,
           uint32_t fec, uint16_t version, uint8_t type)
{
    memset(p, 0, n);
    p[SHRED_OFF_VARIANT] = type;
    memcpy(p + SHRED_OFF_SLOT, &slot, 8);
    memcpy(p + SHRED_OFF_INDEX, &index, 4);
    memcpy(p + SHRED_OFF_VERSION, &version, 2);
    memcpy(p + SHRED_OFF_FEC, &fec, 4);
}

int
main(void)
{
    uint8_t buf[200];
    shred_view_t v;
    shred_identity_t id, id2;

    memset(&id, 0, sizeof(id));
    memset(&id2, 0, sizeof(id2));

    CHECK(shred_parse(NULL, 200, &v) != 0, "null data");
    CHECK(shred_parse(buf, 10, &v) != 0, "too short");
    CHECK(shred_parse(buf, 2000, &v) != 0, "too long");

    fill_shred(buf, 200, 0, 0, 0, 1, 0x00);
    CHECK(shred_parse(buf, 200, &v) != 0, "unknown variant");

    fill_shred(buf, 200, 42, 7, 3, 1, SHRED_TYPE_CHAINED_DATA);
    CHECK(shred_identify(buf, 200, &v, &id) == 0, "chained data");
    CHECK(v.slot == 42 && v.index == 7 && v.fec_set == 3 && v.version == 1,
          "header fields");
    CHECK(shred_is_data(v.type) && !shred_is_code(v.type), "data type");
    CHECK(id.slot == 42 && id.index == 7 && id.fec_set == 3 &&
              id.shred_type == SHRED_TYPE_CHAINED_DATA,
          "identity from header, not payload");

    fill_shred(buf, 200, 42, 7, 3, 1, SHRED_TYPE_CHAINED_CODE);
    CHECK(shred_identify(buf, 89, &v, &id2) == 0, "chained code min size");
    CHECK(shred_is_code(v.type), "code type");
    CHECK(!shred_identity_eq(&id, &id2), "type distinguishes data vs code");

    fill_shred(buf, 200, 42, 7, 3, 1, SHRED_TYPE_CHAINED_DATA);
    memset(buf, 0xFF, 64);
    CHECK(shred_identify(buf, 200, &v, &id2) == 0 && shred_identity_eq(&id, &id2),
          "signature bytes are not part of identity");

    fill_shred(buf, 200, 42, 8, 3, 1, SHRED_TYPE_CHAINED_DATA);
    CHECK(shred_identify(buf, 200, &v, &id2) == 0 && !shred_identity_eq(&id, &id2),
          "index distinguishes shreds");

    {
        uint8_t wrap[250];
        const uint8_t *sp;
        uint16_t slen;

        memset(wrap, 0xCC, sizeof(wrap));
        fill_shred(wrap + 42, 200, 9, 1, 4, 1, SHRED_TYPE_MERKLE_DATA);
        CHECK(shred_envelope(wrap, 242, &sp, &slen) == 0, "udp offset 42");
        CHECK(sp == wrap + 42, "envelope start");
        CHECK(shred_identify_packet(wrap, 242, &v, &id2) == 0 &&
                  id2.slot == 9 && id2.index == 1 &&
                  id2.shred_type == SHRED_TYPE_MERKLE_DATA,
              "identify_packet uses envelope");
    }

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nshred_parse ok\n");
    return 0;
}
