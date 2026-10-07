#include "feed.h"
#include "tsc.h"
#include "util.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct recorder {
    feed_ring_t         *ring;
    feedcap_hdr_t        hdr;
    char                 dir[512];
    char                 prefix[64];
    char                 path[640];
    uint64_t             rotate_bytes;
    uint64_t             file_bytes;
    _Atomic uint64_t     written;
    int                  rec_cpu;
    _Atomic int          run;
    pthread_t            th;
    FILE                *f;
};

int
feed_ring_init(feed_ring_t *r, uint32_t slots)
{
    if (r == NULL || slots < 2u || (slots & (slots - 1u)) != 0) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->slot = calloc(slots, sizeof(feed_slot_t));
    if (r->slot == NULL) {
        return -1;
    }
    r->mask = slots - 1u;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    return 0;
}

void
feed_ring_free(feed_ring_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->slot);
    r->slot = NULL;
}

int
feed_pop(feed_ring_t *r, feed_slot_t *out)
{
    uint64_t head, tail;

    if (r == NULL || out == NULL) {
        return -1;
    }
    tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head) {
        return 1;
    }
    memcpy(out, &r->slot[tail & r->mask], sizeof(*out));
    atomic_store_explicit(&r->tail, tail + 1u, memory_order_release);
    return 0;
}

int
feedcap_write_header(FILE *f, const feedcap_hdr_t *h)
{
    uint8_t buf[FEEDCAP_HDR_LEN];

    if (f == NULL || h == NULL) {
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    memcpy(buf, FEEDCAP_MAGIC, 8);
    memcpy(buf + 8, &h->realtime0_ns, 8);
    memcpy(buf + 16, &h->mono0_ns, 8);
    memcpy(buf + 24, &h->tsc_hz, 8);
    if (fwrite(buf, 1, FEEDCAP_HDR_LEN, f) != FEEDCAP_HDR_LEN) {
        return -1;
    }
    return 0;
}

int
feedcap_write_slot(FILE *f, const feed_slot_t *s)
{
    uint8_t hdr[FEEDCAP_REC_HDR];

    if (f == NULL || s == NULL || s->len > FEED_PKT_MAX) {
        return -1;
    }
    memcpy(hdr, &s->rx_ns, 8);
    memcpy(hdr + 8, &s->rx_tsc, 8);
    memcpy(hdr + 16, &s->len, 4);
    memcpy(hdr + 20, &s->seq, 4);
    if (fwrite(hdr, 1, FEEDCAP_REC_HDR, f) != FEEDCAP_REC_HDR) {
        return -1;
    }
    if (s->len != 0 && fwrite(s->data, 1, s->len, f) != s->len) {
        return -1;
    }
    return 0;
}

int
feedcap_read_header(FILE *f, feedcap_hdr_t *h)
{
    uint8_t buf[FEEDCAP_HDR_LEN];

    if (f == NULL || h == NULL) {
        return -1;
    }
    if (fread(buf, 1, FEEDCAP_HDR_LEN, f) != FEEDCAP_HDR_LEN) {
        return -1;
    }
    if (memcmp(buf, FEEDCAP_MAGIC, 8) != 0) {
        return -1;
    }
    memcpy(&h->realtime0_ns, buf + 8, 8);
    memcpy(&h->mono0_ns, buf + 16, 8);
    memcpy(&h->tsc_hz, buf + 24, 8);
    return 0;
}

int
feedcap_read_slot(FILE *f, feed_slot_t *s)
{
    uint8_t hdr[FEEDCAP_REC_HDR];

    if (f == NULL || s == NULL) {
        return -1;
    }
    if (fread(hdr, 1, FEEDCAP_REC_HDR, f) != FEEDCAP_REC_HDR) {
        return 1;
    }
    memcpy(&s->rx_ns, hdr, 8);
    memcpy(&s->rx_tsc, hdr + 8, 8);
    memcpy(&s->len, hdr + 16, 4);
    memcpy(&s->seq, hdr + 20, 4);
    if (s->len > FEED_PKT_MAX) {
        return -1;
    }
    if (s->len != 0 && fread(s->data, 1, s->len, f) != s->len) {
        return -1;
    }
    return 0;
}

static int
rec_open_file(recorder_t *r)
{
    time_t now;
    struct tm tm;

    now = time(NULL);
    gmtime_r(&now, &tm);
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
    if (feedcap_write_header(r->f, &r->hdr) != 0) {
        fclose(r->f);
        r->f = NULL;
        return -1;
    }
    r->file_bytes = FEEDCAP_HDR_LEN;
    return 0;
}

static void *
rec_thread(void *arg)
{
    recorder_t *r = arg;
    feed_slot_t slot;
    uint32_t batch = 0;

    (void)pin_cpu(r->rec_cpu);
    while (atomic_load_explicit(&r->run, memory_order_acquire) ||
           atomic_load_explicit(&r->ring->head, memory_order_acquire) !=
               atomic_load_explicit(&r->ring->tail, memory_order_relaxed)) {
        int rc = feed_pop(r->ring, &slot);
        if (rc != 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000L };
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
            continue;
        }
        if (r->f == NULL && rec_open_file(r) != 0) {
            continue;
        }
        if (r->rotate_bytes != 0 &&
            r->file_bytes + FEEDCAP_REC_HDR + slot.len >= r->rotate_bytes) {
            fflush(r->f);
            fclose(r->f);
            r->f = NULL;
            if (rec_open_file(r) != 0) {
                continue;
            }
        }
        if (feedcap_write_slot(r->f, &slot) == 0) {
            r->file_bytes += FEEDCAP_REC_HDR + slot.len;
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
recorder_start(recorder_t **out, feed_ring_t *ring,
               const char *dir, const char *prefix,
               uint64_t rotate_bytes, int rec_cpu,
               const feedcap_hdr_t *hdr)
{
    recorder_t *r;

    if (out == NULL || ring == NULL || dir == NULL || prefix == NULL || hdr == NULL) {
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
recorder_stop(recorder_t *rec)
{
    if (rec == NULL) {
        return;
    }
    atomic_store_explicit(&rec->run, 0, memory_order_release);
    pthread_join(rec->th, NULL);
    free(rec);
}

uint64_t
recorder_written(const recorder_t *rec)
{
    if (rec == NULL) {
        return 0;
    }
    return atomic_load_explicit(&rec->written, memory_order_relaxed);
}

const char *
recorder_path(const recorder_t *rec)
{
    if (rec == NULL) {
        return NULL;
    }
    return rec->path;
}
