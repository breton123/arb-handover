#ifndef FIRM_NET_CAPTURE_H
#define FIRM_NET_CAPTURE_H

#include "packet.h"
#include "ring.h"

#include <stdint.h>
#include <stdio.h>

/*
 * FIRMCAP1 — raw ingest capture.
 *
 * Off-path and lossy. The RX thread may try_push a copy into a capture
 * ring; it never waits on disk. A recorder thread drains the ring.
 *
 * Not FEEDCAP1: records carry source_id so raced feeds can be replayed.
 *
 * File header (40 bytes):
 *   magic[8] = "FIRMCAP1"
 *   realtime0_ns u64
 *   mono0_ns     u64
 *   tsc_hz       u64
 *   reserved     u64
 *
 * Record header (28 bytes) + data[len]:
 *   rx_ns u64
 *   rx_tsc u64
 *   len u32
 *   seq u32
 *   source_id u8
 *   flags u8
 *   pad u16
 *   data[len]
 */

#define NETCAP_MAGIC    "FIRMCAP1"
#define NETCAP_HDR_LEN  40u
#define NETCAP_REC_HDR  28u

typedef struct {
    uint64_t realtime0_ns;
    uint64_t mono0_ns;
    uint64_t tsc_hz;
} netcap_hdr_t;

int netcap_write_header(FILE *f, const netcap_hdr_t *h);
int netcap_write_slot(FILE *f, const net_slot_t *s);
int netcap_read_header(FILE *f, netcap_hdr_t *h);
int netcap_read_slot(FILE *f, net_slot_t *s);

typedef struct net_recorder net_recorder_t;

int net_recorder_start(net_recorder_t **out, net_ring_t *ring,
                       const char *dir, const char *prefix,
                       uint64_t rotate_bytes, int rec_cpu,
                       const netcap_hdr_t *hdr);
void net_recorder_stop(net_recorder_t *rec);
uint64_t net_recorder_written(const net_recorder_t *rec);
const char *net_recorder_path(const net_recorder_t *rec);

#endif /* FIRM_NET_CAPTURE_H */
