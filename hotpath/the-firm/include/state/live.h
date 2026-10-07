#ifndef FIRM_STATE_LIVE_H
#define FIRM_STATE_LIVE_H

#include "state/authvec.h"
#include "state/compact.h"
#include "tx/pumpstate.h"
#include "wire/lut_cache.h"
#include "wire/lut_file.h"

#include <stdio.h>
#include <stdint.h>

/*
 * LIVE-STATE-001. Shadow Pump CompactState. AUTH is a referee only:
 * it never gates apply and never writes live rows.
 */

#define LIVE_LAT_MAX  4096u
#define LIVE_TX_KEEP  64u
#define LIVE_TX_CAP   1280u
#define LIVE_DISC_MAX 256u
#define LIVE_FAM_MAX  128u
#define LIVE_CAP_MAX  256u

typedef struct {
    uint8_t  sig[STATE_SIG_LEN];
    uint8_t  bytes[LIVE_TX_CAP];
    uint32_t len;
    uint64_t slot;
    uint8_t  lut_resolved;
    uint8_t  n_pump;
} live_tx_snap_t;

typedef struct {
    uint8_t  disc[8];
    uint8_t  nacc;
    uint16_t dlen;
    uint8_t  sig[STATE_SIG_LEN];
    uint64_t n;
} live_fam_t;

typedef struct {
    compact_state_t *st;
    lut_cache_t     *lut;
    authvec_t       *av;
    pumpstate_out_t  out;
    FILE            *pred_log;
    FILE            *mismatch_log;
    FILE            *lut_miss_log;
    FILE            *disc_log;
    FILE            *dirty_log;
    FILE            *ps_log; /* research: Pump/LUT dispatch journal */
    FILE            *walk_log;
    uint8_t          have_walk;
    uint8_t          walk_pk[32];
    uint8_t          walk_vb[32];
    uint8_t          walk_vq[32];
    uint32_t         walk_pool_id;
    uint64_t         walk_from; /* exclusive of snapshot slot */
    uint64_t         walk_to;
    uint64_t         n_walk;
    uint64_t         n_walk_lut;
    uint64_t         n_walk_touch;
    uint64_t         walk_vb_pre;
    uint64_t         walk_vq_pre;
    const char      *fam_path;
    uint8_t          pre_grade[STATE_POOL_MAX];
    uint8_t          pre_auth[STATE_POOL_MAX];
    uint64_t         pre_anchor[STATE_POOL_MAX];
    uint8_t          dirty_logged[STATE_POOL_MAX];
    uint8_t          pool_kill_rc[STATE_POOL_MAX]; /* first dirty pumpstate rc */
    live_fam_t       fam[LIVE_FAM_MAX];
    uint32_t         n_fam;
    uint32_t         n_first_dirty;
    const char      *lut_learned_path;
    const char      *auth_path;
    uint64_t         auth_off;
    uint8_t          disc_pk[LIVE_DISC_MAX][32];
    uint32_t         n_disc;
    uint64_t         n_framed;
    uint64_t         n_pump;
    uint64_t         n_supported;
    uint64_t         n_applied;
    uint64_t         n_no_quote_tx;
    uint64_t         max_framed; /* 0 = unlimited; research replay cap */
    uint64_t         n_pred_drop;
    uint64_t         n_mismatch_dirty;
    uint64_t         reason[9];
    uint8_t          saw_apply[STATE_POOL_MAX];
    uint8_t          have_trace;
    uint8_t          trace_stop;
    uint8_t          trace_sig[STATE_SIG_LEN];
    uint64_t         n_caught_up;
    uint64_t         lat[LIVE_LAT_MAX];
    uint32_t         nlat;
    pump_state_t     pre_snap[STATE_POOL_MAX];
    live_tx_snap_t   snap[LIVE_TX_KEEP];
    uint32_t         nsnap;
    uint32_t         snap_i;
} live_t;

void live_clear(live_t *L, compact_state_t *st, lut_cache_t *lut,
                authvec_t *av);

int live_open_logs(live_t *L, const char *pred_path, const char *mm_path);
int live_open_lut_miss(live_t *L, const char *path);
int live_open_disc(live_t *L, const char *path);
int live_open_dirty(live_t *L, const char *path);
int live_open_ps_log(live_t *L, const char *path);
int live_open_walk_log(live_t *L, const char *path);
void live_write_fam(const live_t *L);
void live_lut_poll(live_t *L);
void live_auth_poll(live_t *L);
void live_close_logs(live_t *L);

int live_on_framed(live_t *L, const uint8_t *bytes, uint32_t len,
                   uint64_t slot, uint64_t of_rx_tsc);

void live_judge(live_t *L);
void live_print(const live_t *L, FILE *fp);

uint32_t live_n_exact_pools(const compact_state_t *st);
uint32_t live_n_dirty_pools(const compact_state_t *st);

#endif
