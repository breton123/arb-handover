#include "trigger.h"
#include "classify.h"
#include "tx.h"

#include <string.h>
#include <time.h>

enum { R_BAD = -1, R_NEED = 1, R_OK = 0 };

typedef struct {
    const uint8_t *writable_idx;
    const uint8_t *readonly_idx;
    const uint8_t *addrs;
    uint16_t n_writable;
    uint16_t n_readonly;
    uint16_t n_addrs;
} lut_view_t;

static uint64_t
now_ns(void)
{
#if defined(CLOCK_MONOTONIC_RAW)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return 0;
#endif
}

static int
cu16(const uint8_t *p, uint32_t avail, uint16_t *val, uint16_t *used)
{
    return cu16_dec(p, (uint16_t)avail, val, used) == 0 ? R_OK
        : (avail < 1 ? R_NEED : R_BAD);
}

static void
add_hit(trigger_hit_t *out, uint32_t pool_idx, uint8_t proto)
{
    uint8_t i;
    if (out->n_watch >= 8) {
        return;
    }
    for (i = 0; i < out->n_watch; i++) {
        if (out->hit_pool[i] == pool_idx) {
            return;
        }
    }
    out->hit_pool[out->n_watch] = pool_idx;
    out->hit_proto[out->n_watch] = proto;
    out->n_watch++;
}

static int
resolve_loaded(const uint8_t *p, uint32_t len, uint32_t keys_off,
               uint16_t n_static, uint8_t versioned,
               alt_cache_t *alts, uint8_t *loaded, uint16_t *n_loaded,
               const alt_bank_t *bank, trigger_hit_t *out)
{
    uint16_t n = n_static;
    if (n_static > TRIG_LOADED_MAX) {
        return -1;
    }
    memcpy(loaded, p + keys_off, (size_t)n_static * 32u);
    if (versioned != FRAME_VER_V0) {
        /* Legacy and v1: every address is inline. No ALT section. */
        *n_loaded = n_static;
        return 0;
    }
    {
        uint32_t off = keys_off + (uint32_t)n_static * 32u + TX_BLOCKHASH_SZ;
        uint16_t ninstr, used, nlut, j, k;
        uint32_t n_writable = 0, n_readonly = 0;
        lut_view_t luts[TRIG_LUT_MAX];
        int missing = 0;
        int uncertain = 0;
        memset(luts, 0, sizeof(luts));
        if (cu16(p + off, len - off, &ninstr, &used) != R_OK) {
            return -1;
        }
        off += used;
        for (j = 0; j < ninstr; j++) {
            uint16_t nacct, dlen;
            if (off >= len) {
                return -1;
            }
            off++;
            if (cu16(p + off, len - off, &nacct, &used) != R_OK) {
                return -1;
            }
            off += used + nacct;
            if (cu16(p + off, len - off, &dlen, &used) != R_OK) {
                return -1;
            }
            off += used + dlen;
        }
        if (cu16(p + off, len - off, &nlut, &used) != R_OK) {
            return -1;
        }
        off += used;
        if (nlut > TRIG_LUT_MAX) {
            return -1;
        }
        out->nlut = (uint8_t)(nlut > 255 ? 255 : nlut);
        for (j = 0; j < nlut; j++) {
            const uint8_t *altpk;
            const uint8_t *addrs = NULL;
            uint16_t an = 0, nw, nr;
            if (off + 32u > len) {
                return -1;
            }
            altpk = p + off;
            off += 32u;
            if (cu16(p + off, len - off, &nw, &used) != R_OK) {
                return -1;
            }
            off += used;
            if (off + nw > len) {
                return -1;
            }
            luts[j].writable_idx = p + off;
            luts[j].n_writable = nw;
            off += nw;
            if (cu16(p + off, len - off, &nr, &used) != R_OK) {
                return -1;
            }
            off += used;
            if (off + nr > len) {
                return -1;
            }
            luts[j].readonly_idx = p + off;
            luts[j].n_readonly = nr;
            if (alt_cache_get(alts, altpk, &addrs, &an) != 0) {
                if (!missing) {
                    out->alt_miss = 1;
                    memcpy(out->alt_miss_pk, altpk, 32);
                }
                missing = 1;
            } else {
                luts[j].addrs = addrs;
                luts[j].n_addrs = an;
                uint16_t active_n = 0;
                int status = alt_cache_check(alts, altpk, bank, &active_n);
                if (status == ALT_VALID) {
                    for (k = 0; k < nw; k++)
                        if (luts[j].writable_idx[k] >= active_n) status = ALT_INDEX_UNAVAILABLE;
                    for (k = 0; k < nr; k++)
                        if (luts[j].readonly_idx[k] >= active_n) status = ALT_INDEX_UNAVAILABLE;
                } else if (bank != NULL
                           && (status == ALT_BANK_UNPROVEN || status == ALT_META_UNKNOWN)
                           && alt_cache_live_active_n(alts, altpk, bank->slot, &active_n) == 0) {
                    int idx_ok = 1;
                    for (k = 0; k < nw; k++)
                        if (luts[j].writable_idx[k] >= active_n) idx_ok = 0;
                    for (k = 0; k < nr; k++)
                        if (luts[j].readonly_idx[k] >= active_n) idx_ok = 0;
                    if (idx_ok) {
                        out->alt_status = (uint8_t)status;
                        status = ALT_VALID;
                    } else {
                        status = ALT_INDEX_UNAVAILABLE;
                    }
                }
                if (status != ALT_VALID && !uncertain) {
                    uncertain = 1;
                    out->alt_status = (uint8_t)status;
                    if (!missing) memcpy(out->alt_miss_pk, altpk, 32);
                }
            }
            n_writable += nw;
            n_readonly += nr;
            off += nr;
        }
        if (missing) {
            out->alt_status = ALT_MISSING;
            return 1;
        }
        if (uncertain) return 2;
        if (n_writable + n_readonly > TRIG_LOADED_MAX - n_static) {
            return -1;
        }
        /* v0 key order groups all tables' writable addresses before readonly. */
        for (j = 0; j < nlut; j++) {
            for (k = 0; k < luts[j].n_writable; k++) {
                uint8_t idx = luts[j].writable_idx[k];
                if (idx >= luts[j].n_addrs) {
                    return -1;
                }
                memcpy(loaded + (uint32_t)n * 32u,
                       luts[j].addrs + (uint32_t)idx * 32u, 32);
                n++;
            }
        }
        for (j = 0; j < nlut; j++) {
            for (k = 0; k < luts[j].n_readonly; k++) {
                uint8_t idx = luts[j].readonly_idx[k];
                if (idx >= luts[j].n_addrs) {
                    return -1;
                }
                memcpy(loaded + (uint32_t)n * 32u,
                       luts[j].addrs + (uint32_t)idx * 32u, 32);
                n++;
            }
        }
    }
    *n_loaded = n;
    return 0;
}

static void
copy_sig0(const uint8_t *p, uint32_t start, const frame_hit_t *fh, swapix_t *ix)
{
    uint16_t used = 0, nsig = 0;
    uint32_t sig_off;

    if (fh != NULL && fh->versioned == FRAME_VER_V1) {
        if (fh->nsig < 1 || fh->tx_end < (uint32_t)fh->nsig * TX_SIG_SZ) {
            return;
        }
        sig_off = fh->tx_end - (uint32_t)fh->nsig * TX_SIG_SZ;
        if (sig_off + 64u > fh->tx_end) {
            return;
        }
        memcpy(ix->sig, p + sig_off, 64);
        ix->have_sig = 1;
        return;
    }
    if (cu16_dec(p + start, 3, &nsig, &used) != 0 || nsig < 1) {
        return;
    }
    if (start + used + 64u > fh->tx_end) {
        return;
    }
    memcpy(ix->sig, p + start + used, 64);
    ix->have_sig = 1;
}

int
trigger_eval_at(const uint8_t *p, uint32_t len, uint32_t start,
             alt_cache_t *alts, const watch_idx_t *watch,
             const alt_bank_t *bank, trigger_hit_t *out, trigger_metrics_t *m)
{
    frame_hit_t fh;
    uint8_t loaded[TRIG_LOADED_MAX * 32u];
    uint16_t n_loaded = 0;
    uint32_t keys_off;
    uint16_t nsig, used;
    int rc, i;
    uint64_t t0, t1;

    if (p == NULL || out == NULL || alts == NULL || watch == NULL) {
        return TRIG_INVALID;
    }
    memset(out, 0, sizeof(*out));
    t0 = now_ns();
    rc = frame_try_any(p, len, start, &fh);
    t1 = now_ns();
    if (m != NULL && t1 >= t0) {
        m->ns_frame += t1 - t0;
    }
    if (rc == R_NEED) {
        out->klass = TRIG_INCOMPLETE;
        return TRIG_INCOMPLETE;
    }
    if (rc != R_OK) {
        out->klass = TRIG_INVALID;
        return TRIG_INVALID;
    }
    out->versioned = fh.versioned;
    out->tx_off = fh.tx_off;
    out->tx_end = fh.tx_end;
    out->n_static = fh.nkeys;
    {
        swapix_t sig0;
        memset(&sig0, 0, sizeof(sig0));
        copy_sig0(p, start, &fh, &sig0);
        if (sig0.have_sig) {
            memcpy(out->sig, sig0.sig, 64);
            out->have_sig = 1;
        }
    }
    if (m != NULL) {
        m->framed++;
    }
    if (fh.versioned == FRAME_VER_V1) {
        if (start + TX_V1_HDR_SIZE > fh.tx_end) {
            out->klass = TRIG_INVALID;
            return TRIG_INVALID;
        }
        keys_off = start + TX_V1_HDR_SIZE;
        nsig = fh.nsig;
        used = 0;
    } else {
        if (cu16_dec(p + start, 3, &nsig, &used) != 0) {
            out->klass = TRIG_INVALID;
            return TRIG_INVALID;
        }
        keys_off = start + used + (uint32_t)nsig * TX_SIG_SZ;
        if (p[keys_off] & 0x80u) {
            keys_off += 1u + 1u + 2u; /* version, nsig, ro_s, ro_u */
        } else {
            keys_off += 1u + 2u;
        }
        {
            uint16_t nk2, u2;
            if (cu16_dec(p + keys_off, 3, &nk2, &u2) != 0) {
                out->klass = TRIG_INVALID;
                return TRIG_INVALID;
            }
            keys_off += u2;
        }
    }
    t0 = now_ns();
    rc = resolve_loaded(p, len, keys_off, fh.nkeys, fh.versioned, alts,
                        loaded, &n_loaded, bank, out);
    t1 = now_ns();
    if (m != NULL && t1 >= t0) {
        m->ns_alt += t1 - t0;
    }
    if (rc < 0) {
        out->klass = TRIG_INVALID;
        return TRIG_INVALID;
    }
    if (rc == 1 || rc == 2) {
        out->klass = rc == 1 ? TRIG_ALT_MISS : TRIG_ALT_UNCERTAIN;
        if (m != NULL) {
            if (rc == 1) m->alt_miss++;
            else m->alt_uncertain++;
        }
        if (rc == 1) (void)alt_cache_note_miss(alts, out->alt_miss_pk);
        /* Still intersect STATIC keys. */
        n_loaded = fh.nkeys;
        memcpy(loaded, p + keys_off, (size_t)fh.nkeys * 32u);
    } else if (m != NULL) {
        m->alt_hit++;
    }
    out->n_loaded = n_loaded;
    if (rc == 2) {
        swapix_t keep = {0};
        copy_sig0(p, start, &fh, &keep);
        memcpy(out->sig, keep.sig, 64);
        out->have_sig = keep.have_sig;
        return TRIG_ALT_UNCERTAIN;
    }
    if (n_loaded > 0) {
        memcpy(out->outer, loaded, 32);
    }
    t0 = now_ns();
    for (i = 0; i < (int)n_loaded; i++) {
        watch_ent_t we;
        if (watch_get(watch, loaded + (uint32_t)i * 32u, &we) == 0) {
            out->relevant = 1;
            add_hit(out, we.pool_idx, we.protocol);
        }
    }
    t1 = now_ns();
    if (m != NULL && t1 >= t0) {
        m->ns_watch += t1 - t0;
    }
    if (!out->relevant) {
        if (out->alt_miss) {
            out->klass = TRIG_ALT_MISS;
            return TRIG_ALT_MISS;
        }
        out->klass = TRIG_DROP;
        if (m != NULL) {
            m->drop++;
        }
        return TRIG_DROP;
    }
    if (m != NULL) {
        m->relevant++;
    }
    t0 = now_ns();
    if (!out->alt_miss
        && swapix_from_loaded(p + start, (uint16_t)(fh.tx_end - start),
                              (uint16_t)(keys_off - start), fh.nkeys,
                              loaded, n_loaded, &out->exact) == 0) {
        copy_sig0(p, start, &fh, &out->exact);
        if (out->exact.have_sig && out->exact.amount_in != 0) {
            memcpy(out->sig, out->exact.sig, 64);
            out->have_sig = 1;
            out->klass = TRIG_EXACT;
            if (m != NULL) {
                m->exact++;
                m->ns_decode += now_ns() - t0;
            }
            return TRIG_EXACT;
        }
    }
    t1 = now_ns();
    if (m != NULL && t1 >= t0) {
        m->ns_decode += t1 - t0;
        m->unknown++;
    }
    {
        swapix_t keep;
        memset(&keep, 0, sizeof(keep));
        copy_sig0(p, start, &fh, &keep);
        if (keep.have_sig) {
            memcpy(out->sig, keep.sig, 64);
            out->have_sig = 1;
        }
    }
    memset(&out->exact, 0, sizeof(out->exact));
    out->klass = TRIG_RELEVANT_UNKNOWN;
    return TRIG_RELEVANT_UNKNOWN;
}

static int
plausible(const uint8_t *p, uint32_t len, uint32_t off)
{
    uint16_t nsig, used;
    uint32_t msg;
    if (off + 65u > len) {
        return 0;
    }
    if (cu16_dec(p + off, 3, &nsig, &used) != 0) {
        return 0;
    }
    if (nsig < 1 || nsig > TX_SIG_MAX) {
        return 0;
    }
    msg = off + used + (uint32_t)nsig * TX_SIG_SZ;
    if (msg >= len) {
        return 0;
    }
    if ((p[msg] & 0x80u) != 0) {
        return (p[msg] & 0x7fu) == 0;
    }
    return p[msg] == (uint8_t)nsig;
}

int
trigger_scan_at(const uint8_t *p, uint32_t len,
             alt_cache_t *alts, const watch_idx_t *watch,
             const alt_bank_t *bank, trigger_hit_t *out, trigger_metrics_t *m)
{
    uint32_t off;
    uint32_t tries = 0;
    trigger_hit_t best;
    int have = 0;

    if (p == NULL || out == NULL) {
        return -1;
    }
    memset(&best, 0, sizeof(best));
    if (m != NULL) {
        m->scan++;
    }
    for (off = 0; off + 65u <= len && tries < TRIG_SCAN_MAX; off++) {
        trigger_hit_t hit;
        int k;
        int v1 = (p[off] == TX_V1_PREFIX
                  && off + TX_V1_HDR_SIZE <= len
                  && p[off + 1u] >= 1 && p[off + 1u] <= TX_SIG_MAX);
        if (!v1 && !plausible(p, len, off)) {
            continue;
        }
        tries++;
        k = trigger_eval_at(p, len, off, alts, watch, bank, &hit, m);
        if (k == TRIG_EXACT) {
            *out = hit;
            return 0;
        }
        if (!have && (k == TRIG_RELEVANT_UNKNOWN || k == TRIG_ALT_MISS || k == TRIG_ALT_UNCERTAIN)) {
            best = hit;
            have = 1;
        }
    }
    if (have) {
        *out = best;
        return 0;
    }
    return -1;
}

int
trigger_eval(const uint8_t *p, uint32_t len, uint32_t start,
             alt_cache_t *alts, const watch_idx_t *watch,
             trigger_hit_t *out, trigger_metrics_t *m)
{
    return trigger_eval_at(p, len, start, alts, watch, NULL, out, m);
}

int
trigger_admit_at(const uint8_t *p, uint32_t len, const uint8_t signature[64],
                 alt_cache_t *alts, const watch_idx_t *watch,
                 const alt_bank_t *bank, trigger_hit_t *out)
{
    if (!out) return TRIG_INVALID;
    memset(out, 0, sizeof(*out));
    int result = trigger_eval_at(p, len, 0, alts, watch, bank, out, NULL);
    if (!signature || !out->have_sig || memcmp(signature, out->sig, 64)
        || out->tx_end != len || out->tx_off != 0) {
        memset(&out->exact, 0, sizeof(out->exact));
        out->klass = TRIG_INVALID;
        return TRIG_INVALID;
    }
    return result;
}

int
trigger_scan(const uint8_t *p, uint32_t len, alt_cache_t *alts,
             const watch_idx_t *watch, trigger_hit_t *out, trigger_metrics_t *m)
{
    return trigger_scan_at(p, len, alts, watch, NULL, out, m);
}
