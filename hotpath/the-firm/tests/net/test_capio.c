#include "net/capio.h"
#include "net/capture.h"
#include "wire/mkpkt.h"

#include <stdio.h>
#include <stdlib.h>
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
tmp_path(char *out, size_t n, const char *suf)
{
#if defined(_WIN32)
    snprintf(out, n, "%s\\firm_capio_%s",
             getenv("TEMP") != NULL ? getenv("TEMP") : ".", suf);
#else
    snprintf(out, n, "/tmp/firm_capio_%s", suf);
#endif
}

int
main(void)
{
    char firm[256], feed[256];
    capio_t c;
    net_slot_t s, got;
    netcap_hdr_t h;
    FILE *f;
    uint8_t tx[256], pkt[400], pool[32], rec[24];
    uint32_t n;
    uint16_t plen;
    uint64_t ns = 11, tsc = 22;
    uint32_t len32, seq = 7;

    memset(pool, 0x55, 32);
    n = wire_mk_pump_sell(tx, sizeof(tx), pool, 1);
    plen = wire_wrap_shred(pkt, sizeof(pkt), 1, tx, n);
    CHECK(n != 0 && plen != 0, "mkpkt");

    tmp_path(firm, sizeof(firm), "firm.cap");
    tmp_path(feed, sizeof(feed), "feed.cap");

    f = fopen(firm, "wb");
    CHECK(f != NULL, "open firm");
    memset(&h, 0, sizeof(h));
    h.tsc_hz = 3500000000ull;
    h.realtime0_ns = 100;
    CHECK(netcap_write_header(f, &h) == 0, "firm hdr");
    memset(&s, 0, sizeof(s));
    s.rx_ns = 9;
    s.rx_tsc = 99;
    s.seq = 3;
    s.source_id = NET_SOURCE_ORBITFLARE;
    s.len = plen;
    memcpy(s.data, pkt, plen);
    CHECK(netcap_write_slot(f, &s) == 0, "firm slot");
    fclose(f);

    CHECK(capio_open(&c, firm) == 0 && c.kind == CAPIO_FIRM, "open FIRMCAP1");
    CHECK(c.tsc_hz == 3500000000ull, "firm hz");
    CHECK(capio_read(&c, &got) == 0, "read firm");
    CHECK(got.len == plen && got.rx_tsc == 99 && got.seq == 3, "firm rec");
    CHECK(memcmp(got.data, pkt, plen) == 0, "firm bytes");
    CHECK(capio_read(&c, &got) == 1, "firm eof");
    capio_close(&c);

    f = fopen(feed, "wb");
    CHECK(f != NULL, "open feed");
    {
        uint8_t hdr[40];

        memset(hdr, 0, sizeof(hdr));
        memcpy(hdr, "FEEDCAP1", 8);
        memcpy(hdr + 8, &h.realtime0_ns, 8);
        memcpy(hdr + 24, &h.tsc_hz, 8);
        CHECK(fwrite(hdr, 1, 40, f) == 40, "feed hdr");
    }
    memset(rec, 0, sizeof(rec));
    memcpy(rec, &ns, 8);
    memcpy(rec + 8, &tsc, 8);
    len32 = plen;
    memcpy(rec + 16, &len32, 4);
    memcpy(rec + 20, &seq, 4);
    CHECK(fwrite(rec, 1, 24, f) == 24, "feed rec");
    CHECK(fwrite(pkt, 1, plen, f) == plen, "feed payload");
    fclose(f);

    CHECK(capio_open(&c, feed) == 0 && c.kind == CAPIO_FEED, "open FEEDCAP1");
    CHECK(c.tsc_hz == 3500000000ull, "feed hz");
    CHECK(capio_read(&c, &got) == 0, "read feed");
    CHECK(got.len == plen && got.rx_tsc == 22 && got.seq == 7, "feed rec");
    CHECK(got.source_id == NET_SOURCE_ORBITFLARE, "feed source");
    CHECK(capio_read(&c, &got) == 1, "feed eof");
    capio_close(&c);

    remove(firm);
    remove(feed);
    if (g_fail) {
        return 1;
    }
    printf("\ncapio ok\n");
    return 0;
}
