#include "syncrec.h"

#include <string.h>

static int
wr(FILE *f, const void *p, size_t n)
{
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int
wr_u8(FILE *f, uint8_t v)
{
    return wr(f, &v, 1);
}

static int
wr_u16(FILE *f, uint16_t v)
{
    return wr(f, &v, 2);
}

static int
wr_u32(FILE *f, uint32_t v)
{
    return wr(f, &v, 4);
}

static int
wr_u64(FILE *f, uint64_t v)
{
    return wr(f, &v, 8);
}

static int
rec_begin(syncrec_t *s, uint32_t body, uint8_t kind, uint64_t ts_ns)
{
    if (s == NULL || s->f == NULL) {
        return -1;
    }
    if (wr_u32(s->f, 12u + body) != 0
        || wr_u8(s->f, kind) != 0
        || wr_u8(s->f, 0) != 0
        || wr_u8(s->f, 0) != 0
        || wr_u8(s->f, 0) != 0
        || wr_u64(s->f, ts_ns) != 0) {
        return -1;
    }
    return 0;
}

static int
rec_end(syncrec_t *s)
{
    return fflush(s->f) == 0 ? 0 : -1;
}

int
syncrec_open(syncrec_t *s, const char *path, uint64_t tsc_hz)
{
    if (s == NULL || path == NULL) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->f = fopen(path, "wb");
    if (s->f == NULL) {
        return -1;
    }
    s->tsc_hz = tsc_hz;
    if (wr_u32(s->f, SYN_MAGIC) != 0
        || wr_u16(s->f, SYN_VER) != 0
        || wr_u16(s->f, 0) != 0
        || wr_u64(s->f, tsc_hz) != 0) {
        syncrec_close(s);
        return -1;
    }
    return 0;
}

void
syncrec_close(syncrec_t *s)
{
    if (s == NULL) {
        return;
    }
    if (s->f != NULL) {
        fclose(s->f);
    }
    memset(s, 0, sizeof(*s));
}

int
syncrec_state_init(syncrec_t *s, const live_univ_t *u, uint64_t ts_ns)
{
    uint32_t i;
    uint32_t body;

    if (u == NULL) {
        return -1;
    }
    body = 8u + 8u + 4u + u->n * 33u;
    if (rec_begin(s, body, SYN_KIND_INIT, ts_ns) != 0
        || wr_u64(s->f, u->slot) != 0
        || wr_u64(s->f, u->state_version) != 0
        || wr_u32(s->f, u->n) != 0) {
        return -1;
    }
    for (i = 0; i < u->n; i++) {
        if (wr_u8(s->f, u->meta[i].protocol) != 0
            || wr(s->f, u->meta[i].pubkey, 32) != 0) {
            return -1;
        }
    }
    if (rec_end(s) != 0) {
        return -1;
    }
    s->n_init++;
    return 0;
}

int
syncrec_mutation(syncrec_t *s, uint64_t ts_ns,
                 uint64_t before_ver, uint64_t after_ver,
                 uint32_t pool_idx, const swapix_t *n)
{
    uint32_t body = 8u + 8u + 4u + 1u + 64u + 8u + 8u + 1u;

    if (n == NULL) {
        return -1;
    }
    if (rec_begin(s, body, SYN_KIND_MUT, ts_ns) != 0
        || wr_u64(s->f, before_ver) != 0
        || wr_u64(s->f, after_ver) != 0
        || wr_u32(s->f, pool_idx) != 0
        || wr_u8(s->f, n->protocol) != 0
        || wr(s->f, n->sig, 64) != 0
        || wr_u64(s->f, n->amount_in) != 0
        || wr_u64(s->f, n->min_out) != 0
        || wr_u8(s->f, n->direction) != 0) {
        return -1;
    }
    if (rec_end(s) != 0) {
        return -1;
    }
    s->n_mut++;
    return 0;
}

int
syncrec_decision(syncrec_t *s, uint64_t ts_ns,
                 const swapix_t *n, uint32_t pool_idx,
                 uint64_t state_version,
                 uint64_t pred_a, uint64_t pred_b,
                 const opportunity_t *opp)
{
    uint32_t body;
    opportunity_t z;

    if (n == NULL) {
        return -1;
    }
    if (opp == NULL) {
        memset(&z, 0, sizeof(z));
        opp = &z;
    }
    body = 64u + 4u + 1u + 1u + 8u + 8u + 8u + 8u + 8u
         + 4u + 8u + 8u + 8u + 1u + 1u;
    if (rec_begin(s, body, SYN_KIND_DEC, ts_ns) != 0
        || wr(s->f, n->sig, 64) != 0
        || wr_u32(s->f, pool_idx) != 0
        || wr_u8(s->f, n->protocol) != 0
        || wr_u8(s->f, n->direction) != 0
        || wr_u64(s->f, n->amount_in) != 0
        || wr_u64(s->f, n->min_out) != 0
        || wr_u64(s->f, state_version) != 0
        || wr_u64(s->f, pred_a) != 0
        || wr_u64(s->f, pred_b) != 0
        || wr_u32(s->f, opp->route_id) != 0
        || wr_u64(s->f, opp->amount_in) != 0
        || wr_u64(s->f, opp->amount_out) != 0
        || wr_u64(s->f, opp->gross_profit) != 0
        || wr_u8(s->f, opp->direction) != 0
        || wr_u8(s->f, opp->valid) != 0) {
        return -1;
    }
    if (rec_end(s) != 0) {
        return -1;
    }
    s->n_dec++;
    return 0;
}

int
syncrec_exec(syncrec_t *s, uint64_t ts_ns,
             const uint8_t tx_sig[64],
             uint64_t signed_ready_ns, uint64_t send_ns,
             uint8_t result)
{
    uint8_t z[64];

    if (tx_sig == NULL) {
        memset(z, 0, sizeof(z));
        tx_sig = z;
    }
    if (rec_begin(s, 64u + 8u + 8u + 1u, SYN_KIND_EXEC, ts_ns) != 0
        || wr(s->f, tx_sig, 64) != 0
        || wr_u64(s->f, signed_ready_ns) != 0
        || wr_u64(s->f, send_ns) != 0
        || wr_u8(s->f, result) != 0) {
        return -1;
    }
    if (rec_end(s) != 0) {
        return -1;
    }
    s->n_exec++;
    return 0;
}
