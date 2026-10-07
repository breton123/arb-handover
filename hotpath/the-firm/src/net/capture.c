#include "net/capture.h"
#include "net/time.h"
#include "net/util.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct net_recorder {
    net_ring_t      *ring;
    netcap_hdr_t     hdr;
    char             dir[512];
    char             prefix[64];
    char             path[640];
    uint64_t         rotate_bytes;
    uint64_t         file_bytes;
    _Atomic uint64_t written;
    int              rec_cpu;
    _Atomic int      run;
    pthread_t        th;
    FILE            *f;
};

int
netcap_write_header(FILE *f, const netcap_hdr_t *h)
{
    uint8_t buf[NETCAP_HDR_LEN];

    if (f == NULL || h == NULL) {
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    memcpy(buf, NETCAP_MAGIC, 8);
    memcpy(buf + 8, &h->realtime0_ns, 8);
    memcpy(buf + 16, &h->mono0_ns, 8);
    memcpy(buf + 24, &h->tsc_hz, 8);
    if (fwrite(buf, 1, NETCAP_HDR_LEN, f) != NETCAP_HDR_LEN) {
        return -1;
    }
    return 0;
}

int
netcap_write_slot(FILE *f, const net_slot_t *s)
{
    uint8_t hdr[NETCAP_REC_HDR];
    uint32_t len;

    if (f == NULL || s == NULL || s->len > NET_PKT_MAX) {
        return -1;
    }
    len = s->len;
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, &s->rx_ns, 8);
    memcpy(hdr + 8, &s->rx_tsc, 8);
    memcpy(hdr + 16, &len, 4);
    memcpy(hdr + 20, &s->seq, 4);
    hdr[24] = s->source_id;
    hdr[25] = s->flags;
    if (fwrite(hdr, 1, NETCAP_REC_HDR, f) != NETCAP_REC_HDR) {
        return -1;
    }
    if (s->len != 0u && fwrite(s->data, 1, s->len, f) != s->len) {
        return -1;
    }
    return 0;
}

int
netcap_read_header(FILE *f, netcap_hdr_t *h)
{
    uint8_t buf[NETCAP_HDR_LEN];

    if (f == NULL || h == NULL) {
        return -1;
    }
    if (fread(buf, 1, NETCAP_HDR_LEN, f) != NETCAP_HDR_LEN) {
        return -1;
    }
    if (memcmp(buf, NETCAP_MAGIC, 8) != 0) {
        return -1;
    }
    memcpy(&h->realtime0_ns, buf + 8, 8);
    memcpy(&h->mono0_ns, buf + 16, 8);
    memcpy(&h->tsc_hz, buf + 24, 8);
    return 0;
}

int
netcap_read_slot(FILE *f, net_slot_t *s)
{
    uint8_t hdr[NETCAP_REC_HDR];
    uint32_t len;

    if (f == NULL || s == NULL) {
        return -1;
    }
    if (fread(hdr, 1, NETCAP_REC_HDR, f) != NETCAP_REC_HDR) {
        return 1;
    }
    memcpy(&s->rx_ns, hdr, 8);
    memcpy(&s->rx_tsc, hdr + 8, 8);
    memcpy(&len, hdr + 16, 4);
    memcpy(&s->seq, hdr + 20, 4);
    s->source_id = hdr[24];
    s->flags = hdr[25];
    if (len > NET_PKT_MAX) {
        return -1;
    }
    s->len = (uint16_t)len;
    if (s->len != 0u && fread(s->data, 1, s->len, f) != s->len) {
        return -1;
    }
    return 0;
}

static int
rec_open_file(net_recorder_t *r)
{
    time_t now;
    struct tm tm;

    now = time(NULL);
#if defined(__linux__)
    gmtime_r(&now, &tm);
#else
    {
        struct tm *p = gmtime(&now);
        if (p == NULL) {
            return -1;
        }
        tm = *p;
    }
#endif
    snprintf(r->path, sizeof(r->path),
             "%s/%s-%04d%02d%02d-%02d%02d%02d.cap",
             r->dir, r->prefix,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    r->f = fopen(r->path, "wb");
    if (r->f == NULL) {
        perror(r->path);
        return -1;
    }
    if (netcap_write_header(r->f, &r->hdr) != 0) {
        fclose(r->f);
        r->f = NULL;
        return -1;
    }
    r->file_bytes = NETCAP_HDR_LEN;
    return 0;
}

static void *
rec_thread(void *arg)
{
    net_recorder_t *r = arg;
    net_slot_t slot;
    uint32_t batch = 0;

    (void)net_pin_cpu(r->rec_cpu);
    while (atomic_load_explicit(&r->run, memory_order_acquire) ||
           atomic_load_explicit(&r->ring->head, memory_order_acquire) !=
               atomic_load_explicit(&r->ring->tail, memory_order_relaxed)) {
        int rc = net_ring_pop_copy(r->ring, &slot);
        if (rc != 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000L };
            nanosleep(&ts, NULL);
            continue;
        }
        if (r->f == NULL && rec_open_file(r) != 0) {
            continue;
        }
        if (r->rotate_bytes != 0 &&
            r->file_bytes + NETCAP_REC_HDR + slot.len >= r->rotate_bytes) {
            fflush(r->f);
            fclose(r->f);
            r->f = NULL;
            if (rec_open_file(r) != 0) {
                continue;
            }
        }
        if (netcap_write_slot(r->f, &slot) == 0) {
            r->file_bytes += NETCAP_REC_HDR + (uint64_t)slot.len;
            atomic_fetch_add_explicit(&r->written, 1, memory_order_relaxed);
            batch++;
            if (batch >= 64u) {
                fflush(r->f);
                batch = 0;
            }
        }
    }
    if (r->f != NULL) {
        fflush(r->f);
        fclose(r->f);
        r->f = NULL;
    }
    return NULL;
}

int
net_recorder_start(net_recorder_t **out, net_ring_t *ring,
                   const char *dir, const char *prefix,
                   uint64_t rotate_bytes, int rec_cpu,
                   const netcap_hdr_t *hdr)
{
    net_recorder_t *r;

    if (out == NULL || ring == NULL || dir == NULL || prefix == NULL ||
        hdr == NULL) {
        return -1;
    }
    r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return -1;
    }
    r->ring = ring;
    r->hdr = *hdr;
    r->rotate_bytes = rotate_bytes;
    r->rec_cpu = rec_cpu;
    snprintf(r->dir, sizeof(r->dir), "%s", dir);
    snprintf(r->prefix, sizeof(r->prefix), "%s", prefix);
    atomic_store(&r->run, 1);
    if (rec_open_file(r) != 0) {
        free(r);
        return -1;
    }
    if (pthread_create(&r->th, NULL, rec_thread, r) != 0) {
        fclose(r->f);
        free(r);
        return -1;
    }
    *out = r;
    return 0;
}

void
net_recorder_stop(net_recorder_t *rec)
{
    if (rec == NULL) {
        return;
    }
    atomic_store_explicit(&rec->run, 0, memory_order_release);
    pthread_join(rec->th, NULL);
    free(rec);
}

uint64_t
net_recorder_written(const net_recorder_t *rec)
{
    if (rec == NULL) {
        return 0;
    }
    return atomic_load_explicit(&rec->written, memory_order_relaxed);
}

const char *
net_recorder_path(const net_recorder_t *rec)
{
    if (rec == NULL) {
        return NULL;
    }
    return rec->path;
}
