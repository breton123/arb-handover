#include "state003.h"

#include "dlmm_cache.h"
#include "proto.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t
mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int
cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

void
state_mgr_free(state_mgr_t *m)
{
    if (m == NULL) {
        return;
    }
    free(m->health);
    free(m->auth_slot);
    free(m->auth_ns);
    free(m->spec_since_ns);
    m->health = NULL;
    m->auth_slot = NULL;
    m->auth_ns = NULL;
    m->spec_since_ns = NULL;
    m->cap = 0;
}

void
state_mgr_init(state_mgr_t *m, const live_univ_t *u)
{
    uint32_t i;

    memset(m, 0, sizeof(*m));
    if (u == NULL || u->n == 0) {
        return;
    }
    m->cap = u->n;
    m->health = calloc(m->cap, sizeof(*m->health));
    m->auth_slot = calloc(m->cap, sizeof(*m->auth_slot));
    m->auth_ns = calloc(m->cap, sizeof(*m->auth_ns));
    m->spec_since_ns = calloc(m->cap, sizeof(*m->spec_since_ns));
    if (m->health == NULL || m->auth_slot == NULL
        || m->auth_ns == NULL || m->spec_since_ns == NULL) {
        state_mgr_free(m);
        return;
    }
    for (i = 0; i < u->n; i++) {
        if (u->meta[i].protocol == PROTO_DLMM) {
            m->health[i] = dlmm_cache_get(&u->dlmm, i) != NULL
                ? ST3_HEALTH_SYNCED : ST3_HEALTH_MISSING;
        } else if (u->meta[i].protocol == PROTO_PUMP) {
            m->health[i] = u->pump_live[i] ? ST3_HEALTH_SYNCED : ST3_HEALTH_MISSING;
        } else {
            m->health[i] = ST3_HEALTH_MISSING;
        }
        m->auth_slot[i] = u->slot;
        /* Authoritative bootstrap is sendable. auth_ns=0 was a false
         * "must land first" gate. Stay sendable until confirm. */
        if (m->health[i] == ST3_HEALTH_SYNCED) {
            uint64_t now = mono_ns();
            m->auth_ns[i] = now != 0 ? now : 1;
        } else {
            m->auth_ns[i] = 0;
        }
    }
}

int
state_sendable(const state_mgr_t *m, const live_univ_t *u, uint32_t pool_idx)
{
    if (m == NULL || u == NULL || pool_idx >= u->n || pool_idx >= m->cap) {
        return 0;
    }
    return m->health[pool_idx] == ST3_HEALTH_SYNCED;
}

void
state_enqueue(state_mgr_t *m, uint32_t pool_idx, int32_t bin_id, uint8_t reason)
{
    st3_need_t *e;
    if (m == NULL || m->q_n >= ST3_Q_CAP) {
        return;
    }
    e = &m->q[m->q_n++];
    e->pool_idx = pool_idx;
    e->bin_id = bin_id;
    e->reason = reason;
    m->n_enqueue++;
}

void
state_mark_stale(state_mgr_t *m, uint32_t pool_idx)
{
    if (m == NULL || pool_idx >= m->cap) {
        return;
    }
    m->health[pool_idx] = ST3_HEALTH_STALE;
}

static int
dlmm_can_apply(const live_univ_t *u, uint32_t pool_idx, const hot_trigger_t *n,
               int32_t *need_bin)
{
    dlmm_pool_hot_t spec;
    dlmm_apply_result_t res;
    const dlmm_pool_hot_t *h;

    if (need_bin != NULL) {
        *need_bin = 0;
    }
    h = dlmm_cache_get(&u->dlmm, pool_idx);
    if (h == NULL) {
        return -1;
    }
    if (dlmm_cache_predict(&u->dlmm, pool_idx, &n->dlmm, &spec, &res) == 0) {
        return 0;
    }
    if (need_bin != NULL) {
        *need_bin = h->active_id;
        if (n->dlmm.swap_for_y) {
            *need_bin = h->active_id - (int32_t)DLMM_CACHE_K - 1;
        } else {
            *need_bin = h->active_id + (int32_t)DLMM_CACHE_K + 1;
        }
    }
    return -1;
}

int
state_observe(state_mgr_t *m, const live_univ_t *u, uint32_t pool_idx,
              const hot_trigger_t *n, hot_decision_t *dec)
{
    uint64_t ver;
    int32_t need = 0;

    if (m == NULL || u == NULL || n == NULL || dec == NULL) {
        return -1;
    }
    memset(dec, 0, sizeof(*dec));
    if (pool_idx >= u->n || pool_idx >= m->cap) {
        return -1;
    }
    ver = u->state_version;
    m->n_observe++;
    if (m->health[pool_idx] == ST3_HEALTH_SPECULATIVE) {
        m->n_trig_speculative++;
    }
    if (m->health[pool_idx] == ST3_HEALTH_MISSING
        || m->health[pool_idx] == ST3_HEALTH_STALE) {
        return ST3_UNHEALTHY;
    }
    if (n->protocol == PROTO_DLMM
        && u->meta[pool_idx].protocol == PROTO_DLMM
        && dlmm_can_apply(u, pool_idx, n, &need) != 0) {
        state_enqueue(m, pool_idx, need, ST3_MISSING_BIN);
        dec->state_version = ver;
        return ST3_MISSING_BIN;
    }
    if (n->protocol == PROTO_PUMP
        && u->meta[pool_idx].protocol == PROTO_PUMP) {
        pump_state_t after;
        pump_swap_result_t res;
        if (!u->pump_live[pool_idx]
            || pump_apply_swap(&u->pump[pool_idx], &n->pump, &after, &res) != 0) {
            dec->state_version = ver;
            return ST3_APPLY_FAIL;
        }
    }
    if (hot_decide(u, pool_idx, n, dec) != 0) {
        return -1;
    }
    if (u->state_version != ver) {
        return -1;
    }
    if (dec->opp.valid && m->health[pool_idx] != ST3_HEALTH_SYNCED) {
        m->n_opp_suppressed++;
    }
    return dec->opp.valid ? ST3_OK : ST3_NO_OPP;
}

int
state_confirm(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx,
              const hot_trigger_t *n)
{
    if (m == NULL || u == NULL || n == NULL || pool_idx >= u->n
        || pool_idx >= m->cap) {
        return -1;
    }
    if (hot_commit(u, pool_idx, n) != 0) {
        state_mark_stale(m, pool_idx);
        return ST3_APPLY_FAIL;
    }
    /* Local apply is not chain truth. Not sendable until refresh. */
    m->health[pool_idx] = ST3_HEALTH_SPECULATIVE;
    m->spec_since_ns[pool_idx] = mono_ns();
    m->n_confirm++;
    m->n_refresh_req++;
    return 0;
}

int
state_reject(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx)
{
    if (m == NULL || u == NULL || pool_idx >= u->n) {
        return -1;
    }
    (void)u;
    m->n_reject++;
    return 0;
}

int
state_refresh(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx,
              const dlmm_state_t *auth, uint64_t slot)
{
    if (m == NULL || u == NULL || auth == NULL || pool_idx >= u->n
        || pool_idx >= m->cap) {
        return -1;
    }
    if (u->meta[pool_idx].protocol != PROTO_DLMM) {
        return -1;
    }
    if (dlmm_cache_put(&u->dlmm, pool_idx, auth) != 0) {
        m->health[pool_idx] = ST3_HEALTH_MISSING;
        return -1;
    }
    u->state_version++;
    u->slot = slot;
    m->health[pool_idx] = ST3_HEALTH_SYNCED;
    m->auth_slot[pool_idx] = slot;
    m->auth_ns[pool_idx] = mono_ns();
    if (m->spec_since_ns[pool_idx] != 0) {
        uint64_t dt = mono_ns() - m->spec_since_ns[pool_idx];
        if (m->n_resync < ST3_RESYNC_CAP && dt <= UINT32_MAX) {
            m->resync_ns[m->n_resync++] = (uint32_t)dt;
        }
        m->spec_since_ns[pool_idx] = 0;
    }
    m->n_refresh++;
    return 0;
}

int
state_refresh_pump(state_mgr_t *m, live_univ_t *u, uint32_t pool_idx,
                   const pump_state_t *auth, uint64_t slot)
{
    if (m == NULL || u == NULL || auth == NULL || pool_idx >= u->n
        || pool_idx >= m->cap) {
        return -1;
    }
    if (u->meta[pool_idx].protocol != PROTO_PUMP) {
        return -1;
    }
    u->pump[pool_idx] = *auth;
    u->pump_live[pool_idx] = 1;
    u->state_version++;
    u->slot = slot;
    m->health[pool_idx] = ST3_HEALTH_SYNCED;
    m->auth_slot[pool_idx] = slot;
    m->auth_ns[pool_idx] = mono_ns();
    if (m->spec_since_ns[pool_idx] != 0) {
        uint64_t dt = mono_ns() - m->spec_since_ns[pool_idx];
        if (m->n_resync < ST3_RESYNC_CAP && dt <= UINT32_MAX) {
            m->resync_ns[m->n_resync++] = (uint32_t)dt;
        }
        m->spec_since_ns[pool_idx] = 0;
    }
    m->n_refresh++;
    return 0;
}

int
state_recon_eq_dlmm(const live_univ_t *u, uint32_t pool_idx,
                    const dlmm_state_t *auth)
{
    const dlmm_pool_hot_t *h;
    dlmm_pool_hot_t ah;

    if (u == NULL || auth == NULL || pool_idx >= u->n) {
        return 0;
    }
    h = dlmm_cache_get(&u->dlmm, pool_idx);
    if (h == NULL || dlmm_state_into_hot(auth, &ah) != 0) {
        return 0;
    }
    return dlmm_hot_econ_eq(h, &ah);
}

int
state_recon_eq_pump(const live_univ_t *u, uint32_t pool_idx,
                    const pump_state_t *auth)
{
    const pump_state_t *p;

    if (u == NULL || auth == NULL || pool_idx >= u->n) {
        return 0;
    }
    if (!u->pump_live[pool_idx]) {
        return 0;
    }
    p = &u->pump[pool_idx];
    return p->reserve_base == auth->reserve_base
        && p->reserve_quote == auth->reserve_quote;
}

uint32_t
state_resync_pct(const state_mgr_t *m, double p)
{
    uint32_t tmp[ST3_RESYNC_CAP];
    uint32_t n;
    uint32_t i;

    if (m == NULL || m->n_resync == 0) {
        return 0;
    }
    n = m->n_resync;
    memcpy(tmp, m->resync_ns, (size_t)n * sizeof(tmp[0]));
    qsort(tmp, n, sizeof(tmp[0]), cmp_u32);
    i = (uint32_t)(p * (double)(n - 1u));
    return tmp[i];
}

void
state_metrics_print(const state_mgr_t *m, FILE *f)
{
    if (m == NULL || f == NULL) {
        return;
    }
    fprintf(f,
            "  state  confirm=%" PRIu64 " reject=%" PRIu64
            " refresh_req=%" PRIu64 " refresh=%" PRIu64
            " exact=%" PRIu64 " mismatch=%" PRIu64
            " spec_trig=%" PRIu64 " opp_suppressed=%" PRIu64
            " resync_us p50=%" PRIu32 " p90=%" PRIu32 " p99=%" PRIu32
            " n=%" PRIu32 "\n",
            m->n_confirm, m->n_reject, m->n_refresh_req, m->n_refresh,
            m->n_exact, m->n_mismatch,
            m->n_trig_speculative, m->n_opp_suppressed,
            state_resync_pct(m, 0.50) / 1000u,
            state_resync_pct(m, 0.90) / 1000u,
            state_resync_pct(m, 0.99) / 1000u,
            m->n_resync);
}
