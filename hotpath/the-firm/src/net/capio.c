#include "net/capio.h"
#include "net/capture.h"

#include <string.h>

#define FEEDCAP_MAGIC   "FEEDCAP1"
#define FEEDCAP_REC_HDR 24u

int
capio_open(capio_t *c, const char *path)
{
    uint8_t mag[8];

    if (c == NULL || path == NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->f = fopen(path, "rb");
    if (c->f == NULL) {
        return -1;
    }
    if (fread(mag, 1, 8, c->f) != 8) {
        fclose(c->f);
        c->f = NULL;
        return -1;
    }
    if (fseek(c->f, 0, SEEK_SET) != 0) {
        fclose(c->f);
        c->f = NULL;
        return -1;
    }
    if (memcmp(mag, NETCAP_MAGIC, 8) == 0) {
        netcap_hdr_t h;

        c->kind = CAPIO_FIRM;
        if (netcap_read_header(c->f, &h) != 0) {
            fclose(c->f);
            c->f = NULL;
            return -1;
        }
        c->tsc_hz = h.tsc_hz;
        c->realtime0_ns = h.realtime0_ns;
        return 0;
    }
    if (memcmp(mag, FEEDCAP_MAGIC, 8) == 0) {
        uint8_t buf[40];

        c->kind = CAPIO_FEED;
        if (fread(buf, 1, 40, c->f) != 40) {
            fclose(c->f);
            c->f = NULL;
            return -1;
        }
        memcpy(&c->realtime0_ns, buf + 8, 8);
        memcpy(&c->tsc_hz, buf + 24, 8);
        return 0;
    }
    fclose(c->f);
    c->f = NULL;
    return -1;
}

void
capio_close(capio_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->f != NULL) {
        fclose(c->f);
    }
    memset(c, 0, sizeof(*c));
}

int
capio_read(capio_t *c, net_slot_t *s)
{
    if (c == NULL || c->f == NULL || s == NULL) {
        return -1;
    }
    if (c->kind == CAPIO_FIRM) {
        return netcap_read_slot(c->f, s);
    }
    {
        uint8_t hdr[FEEDCAP_REC_HDR];
        uint32_t len;

        if (fread(hdr, 1, FEEDCAP_REC_HDR, c->f) != FEEDCAP_REC_HDR) {
            return 1;
        }
        memset(s, 0, sizeof(*s));
        memcpy(&s->rx_ns, hdr, 8);
        memcpy(&s->rx_tsc, hdr + 8, 8);
        memcpy(&len, hdr + 16, 4);
        memcpy(&s->seq, hdr + 20, 4);
        s->source_id = NET_SOURCE_ORBITFLARE;
        if (len > NET_PKT_MAX) {
            return -1;
        }
        s->len = (uint16_t)len;
        if (s->len != 0 && fread(s->data, 1, s->len, c->f) != s->len) {
            return -1;
        }
    }
    return 0;
}
