#ifndef FIRM_NET_CAPIO_H
#define FIRM_NET_CAPIO_H

#include "packet.h"

#include <stdint.h>
#include <stdio.h>

/*
 * Read FIRMCAP1 or historical FEEDCAP1 (OrbitFlare recorder).
 *
 * FEEDCAP1 file header (40 bytes): magic, realtime0_ns, unused, tsc_hz,
 * reserved. Records are 24 bytes (rx_ns, rx_tsc, len, seq) + payload.
 */

#define CAPIO_FIRM  1u
#define CAPIO_FEED  2u

typedef struct {
    FILE    *f;
    uint8_t  kind;
    uint64_t tsc_hz;
    uint64_t realtime0_ns;
} capio_t;

int capio_open(capio_t *c, const char *path);
void capio_close(capio_t *c);
/* 0 ok, 1 eof, -1 error */
int capio_read(capio_t *c, net_slot_t *s);

#endif /* FIRM_NET_CAPIO_H */
