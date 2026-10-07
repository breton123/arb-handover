#include "state/live.h"

#include "deps/class.h"
#include "transitions/overlay.h"
#include "wire/lut_cache.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static uint64_t
rdtsc_now(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int aux;

    return (uint64_t)__builtin_ia32_rdtscp(&aux);
#else
    return 0;
#endif
}

static int
is_pump_fast(uint8_t kind)
{
    return kind == IX_KIND_PUMP_SELL || kind == IX_KIND_PUMP_BUY_EQ
        || kind == IX_KIND_PUMP_BUY;
}

static void
hex_write(FILE *fp, const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        fprintf(fp, "%02x", p[i]);
    }
}

static void
keep_tx(live_t *L, const uint8_t *bytes, uint32_t len, uint64_t slot)
{
    live_tx_snap_t *s;

    if (len > LIVE_TX_CAP) {
        len = LIVE_TX_CAP;
    }
    s = &L->snap[L->snap_i];
    memset(s, 0, sizeof(*s));
    memcpy(s->sig, L->out.tx.sig, STATE_SIG_LEN);
    memcpy(s->bytes, bytes, len);
    s->len = len;
    s->slot = slot;
    s->lut_resolved = L->out.lut_resolved;
    s->n_pump = L->out.n_pump;
    L->snap_i = (L->snap_i + 1u) % LIVE_TX_KEEP;
    if (L->nsnap < LIVE_TX_KEEP) {
        L->nsnap++;
    }
}

static const live_tx_snap_t *
find_snap(const live_t *L, const uint8_t sig[STATE_SIG_LEN])
{
    uint32_t i;

    for (i = 0; i < L->nsnap; i++) {
        if (memcmp(L->snap[i].sig, sig, STATE_SIG_LEN) == 0) {
            return &L->snap[i];
        }
    }
    return NULL;
}

static void
count_supported(live_t *L)
{
    uint16_t i, nf = 0, no = 0;

    for (i = 0; i < L->out.tx.n_ix; i++) {
        if (L->out.tx.ix[i].proto != PROTO_PUMP) {
            continue;
        }
        if (is_pump_fast(L->out.tx.ix[i].kind)) {
            nf++;
        } else {
            no++;
        }
    }
    if (no == 0 && nf != 0) {
        L->n_supported++;
    }
}

static void
log_pred(live_t *L, uint32_t pool_id, const pump_state_t *after,
         uint64_t of_rx, uint64_t ready)
{
    const auth_pred_t *p;
    uint32_t i;

    if (L->pred_log == NULL || L->av->n_pred == 0) {
        return;
    }
    i = L->av->n_pred - 1u;
    p = &L->av->pred[i];
    fprintf(L->pred_log, "{\"slot\":%" PRIu64 ",\"sig_hex\":\"", p->slot);
    hex_write(L->pred_log, p->sig, STATE_SIG_LEN);
    fprintf(L->pred_log,
            "\",\"pool_id\":%" PRIu32 ",\"incarnation\":%" PRIu64
            ",\"prestate_hash\":\"",
            pool_id, p->incarnation);
    hex_write(L->pred_log, p->prestate_hash, 32);
    fprintf(L->pred_log,
            "\",\"reserve_base\":%" PRIu64 ",\"reserve_quote\":%" PRIu64
            ",\"virtual_quote\":%" PRId64 ",\"lp_fee_bps\":%" PRIu64
            ",\"protocol_fee_bps\":%" PRIu64 ",\"creator_fee_bps\":%" PRIu64
            ",\"disabled\":%u,\"status\":%u"
            ",\"of_rx_tsc\":%" PRIu64 ",\"state_ready_tsc\":%" PRIu64 "}\n",
            after->reserve_base, after->reserve_quote, after->virtual_quote,
            after->lp_fee_bps, after->protocol_fee_bps, after->creator_fee_bps,
            after->disabled, after->status, of_rx, ready);
    fflush(L->pred_log);
}

static void
dump_mismatch(live_t *L, const auth_pred_t *p)
{
    const live_tx_snap_t *sn;

    if (L->mismatch_log == NULL) {
        return;
    }
    sn = find_snap(L, p->sig);
    fprintf(L->mismatch_log, "MISMATCH slot=%" PRIu64 " pool_id=%" PRIu32
            " incarnation=%" PRIu64 "\nsig=",
            p->slot, p->pool_id, p->incarnation);
    hex_write(L->mismatch_log, p->sig, STATE_SIG_LEN);
    fprintf(L->mismatch_log,
            "\nprestate rb=%" PRIu64 " rq=%" PRIu64 " vq=%" PRId64
            " lp=%" PRIu64 " proto=%" PRIu64 " creator=%" PRIu64
            " dis=%u st=%u\npredicted rb=%" PRIu64 " rq=%" PRIu64
            " vq=%" PRId64 " lp=%" PRIu64 " proto=%" PRIu64 " creator=%" PRIu64
            " dis=%u st=%u\n",
            p->prestate.reserve_base, p->prestate.reserve_quote,
            p->prestate.virtual_quote, p->prestate.lp_fee_bps,
            p->prestate.protocol_fee_bps, p->prestate.creator_fee_bps,
            p->prestate.disabled, p->prestate.status,
            p->predicted.reserve_base, p->predicted.reserve_quote,
            p->predicted.virtual_quote, p->predicted.lp_fee_bps,
            p->predicted.protocol_fee_bps, p->predicted.creator_fee_bps,
            p->predicted.disabled, p->predicted.status);
    if (sn != NULL) {
        fprintf(L->mismatch_log, "lut_resolved=%u n_pump=%u tx_hex=",
                sn->lut_resolved, sn->n_pump);
        hex_write(L->mismatch_log, sn->bytes, sn->len);
        fputc('\n', L->mismatch_log);
    }
    fflush(L->mismatch_log);
}

void
live_clear(live_t *L, compact_state_t *st, lut_cache_t *lut, authvec_t *av)
{
    if (L == NULL) {
        return;
    }
    memset(L, 0, sizeof(*L));
    L->st = st;
    L->lut = lut;
    L->av = av;
}

int
live_open_lut_miss(live_t *L, const char *path)
{
    if (L == NULL || path == NULL) {
        return -1;
    }
    L->lut_miss_log = fopen(path, "a");
    return (L->lut_miss_log == NULL) ? -1 : 0;
}

int
live_open_disc(live_t *L, const char *path)
{
    if (L == NULL || path == NULL) {
        return -1;
    }
    L->disc_log = fopen(path, "a");
    return (L->disc_log == NULL) ? -1 : 0;
}

static void
note_disc(live_t *L)
{
    uint16_t i;
    uint32_t d;

    if (L->disc_log == NULL || L->out.n_pump == 0 || L->out.lut_resolved == 0) {
        return;
    }
    for (i = 0; i < L->out.tx.n_ix; i++) {
        const ordered_ix_t *ix = &L->out.tx.ix[i];
        uint8_t n, ai;
        const uint8_t *pk;

        if (ix->proto != PROTO_PUMP || !is_pump_fast(ix->kind)) {
            continue;
        }
        n = L->out.tx.acc_n[i];
        if (n == 0) {
            continue;
        }
        ai = L->out.tx.acc_ix[i][0];
        if (ai >= L->out.tx.n_keys) {
            continue;
        }
        pk = L->out.tx.key[ai];
        for (d = 0; d < L->n_disc; d++) {
            if (memcmp(L->disc_pk[d], pk, 32) == 0) {
                pk = NULL;
                break;
            }
        }
        if (pk == NULL) {
            continue;
        }
        if (L->n_disc >= LIVE_DISC_MAX) {
            return;
        }
        memcpy(L->disc_pk[L->n_disc], pk, 32);
        L->n_disc++;
        hex_write(L->disc_log, pk, 32);
        fputc('\n', L->disc_log);
        fflush(L->disc_log);
    }
}

int
live_open_dirty(live_t *L, const char *path)
{
    if (L == NULL || path == NULL) {
        return -1;
    }
    L->dirty_log = fopen(path, "w");
    return (L->dirty_log == NULL) ? -1 : 0;
}

int
live_open_ps_log(live_t *L, const char *path)
{
    if (L == NULL || path == NULL) {
        return -1;
    }
    L->ps_log = fopen(path, "w");
    return (L->ps_log == NULL) ? -1 : 0;
}

int
live_open_walk_log(live_t *L, const char *path)
{
    if (L == NULL || path == NULL) {
        return -1;
    }
    L->walk_log = fopen(path, "w");
    return (L->walk_log == NULL) ? -1 : 0;
}

static const char *
grade_name(uint8_t g)
{
    if (g == POOL_GRADE_EXACT) {
        return "EXACT";
    }
    if (g == POOL_GRADE_DIRTY) {
        return "DIRTY";
    }
    return "UNKNOWN";
}

static void
log_ps_pool(live_t *L, uint32_t id, int *first)
{
    const state_pool_t *row;
    uint8_t gpre, gpost, auth;
    uint64_t anc;
    int kill;

    if (id >= STATE_POOL_MAX || compact_pool_get(L->st, id, &row) != 0) {
        return;
    }
    gpre = L->pre_grade[id];
    auth = L->pre_auth[id];
    anc = L->pre_anchor[id];
    gpost = row->grade;
    kill = L->pool_kill_rc[id];
    if (*first == 0) {
        fputc(',', L->ps_log);
    }
    *first = 0;
    fprintf(L->ps_log,
            "{\"id\":%" PRIu32 ",\"pk_hex\":\"", id);
    hex_write(L->ps_log, row->pubkey, 32);
    fprintf(L->ps_log,
            "\",\"grade_before\":\"%s\",\"grade_after\":\"%s\""
            ",\"auth_ready\":%u,\"auth_bits\":%u,\"anchor_slot\":%" PRIu64
            ",\"prev_kill\":\"%s\"}",
            grade_name(gpre), grade_name(gpost),
            (unsigned)(auth == POOL_AUTH_READY), (unsigned)auth, anc,
            kill == 0 ? "" : pumpstate_name(kill));
}

static void
log_ps_event(live_t *L, int rc, uint64_t slot)
{
    uint16_t i;
    uint32_t id, seen[8], nseen = 0, s;
    int first = 1, want, hit;
    uint8_t logged[STATE_POOL_MAX];

    if (L->ps_log == NULL || L->st == NULL) {
        return;
    }
    want = (L->out.n_pump != 0) || (rc == PS_LUT) || (rc == PS_APPLY);
    if (!want) {
        return;
    }
    memset(logged, 0, sizeof(logged));
    fprintf(L->ps_log,
            "{\"slot\":%" PRIu64 ",\"rc\":\"%s\",\"lut_resolved\":%u"
            ",\"n_pump\":%u,\"n_static\":%u,\"n_alt\":%u"
            ",\"n_ix\":%u,\"dep_class\":%u,\"apply_code\":%d"
            ",\"sig_hex\":\"",
            slot, pumpstate_name(rc), (unsigned)L->out.lut_resolved,
            (unsigned)L->out.n_pump, (unsigned)L->out.view.n_static,
            (unsigned)L->out.view.n_alt, (unsigned)L->out.tx.n_ix,
            (unsigned)L->out.ar.dep_class, L->out.ar.code);
    hex_write(L->ps_log, L->out.view.sig, STATE_SIG_LEN);
    fprintf(L->ps_log, "\",\"pools\":[");
    for (i = 0; i < L->out.tx.n_ix && nseen < 8u; i++) {
        if (L->out.tx.ix[i].proto != PROTO_PUMP) {
            continue;
        }
        id = L->out.tx.ix[i].pool_id;
        if (id == STATE_ACCT_NONE || id >= STATE_POOL_MAX || logged[id]) {
            continue;
        }
        logged[id] = 1;
        seen[nseen++] = id;
        log_ps_pool(L, id, &first);
    }
    if (rc == PS_LUT) {
        for (i = 0; i < L->out.view.n_static && nseen < 8u; i++) {
            if (compact_pool_find(L->st, L->out.view.key[i], &id) != 0) {
                continue;
            }
            if (id >= STATE_POOL_MAX || logged[id]) {
                continue;
            }
            logged[id] = 1;
            seen[nseen++] = id;
            log_ps_pool(L, id, &first);
        }
    }
    hit = 0;
    for (s = 0; s < nseen; s++) {
        if (L->pre_grade[seen[s]] == POOL_GRADE_EXACT
            && L->st->pool[seen[s]].grade == POOL_GRADE_DIRTY) {
            hit = 1;
            break;
        }
    }
    fprintf(L->ps_log, "],\"killed_exact\":%u}\n", (unsigned)hit);
    fflush(L->ps_log);
}

static const char *
first_dirty_cause(int rc, const apply_result_t *ar)
{
    if (rc == PS_LUT) {
        return "LUT_STATIC";
    }
    if (rc == PS_UNSUPPORTED) {
        return "UNKNOWN_PUMP_DISC";
    }
    if (rc == PS_APPLY) {
        if (ar != NULL && ar->overlay.status == OV_UNKNOWN
            && !deps_blocks_fast(ar->dep_class)
            && ar->dep_class != DEP_UNKNOWN) {
            return "UNKNOWN_TOKEN_EFFECT";
        }
        if (ar != NULL) {
            return deps_class_name(ar->dep_class);
        }
        return "APPLY_REJECT";
    }
    return "OTHER";
}

static void
note_fam(live_t *L)
{
    uint16_t i;

    if (L->out.n_pump == 0 || L->out.view.n_ix == 0) {
        return;
    }
    for (i = 0; i < L->out.tx.n_ix && i < L->out.view.n_ix; i++) {
        const ordered_ix_t *ix = &L->out.tx.ix[i];
        const tx_ix_t *vx = &L->out.view.ix[i];
        const uint8_t *d;
        uint32_t f;
        uint16_t dl;

        if (ix->proto != PROTO_PUMP) {
            continue;
        }
        d = vx->data;
        dl = vx->dlen;
        if (d == NULL || dl < 8u) {
            continue;
        }
        for (f = 0; f < L->n_fam; f++) {
            if (memcmp(L->fam[f].disc, d, 8) == 0) {
                L->fam[f].n++;
                break;
            }
        }
        if (f == L->n_fam) {
            if (L->n_fam >= LIVE_FAM_MAX) {
                continue;
            }
            memset(&L->fam[f], 0, sizeof(L->fam[f]));
            memcpy(L->fam[f].disc, d, 8);
            L->fam[f].dlen = dl;
            L->fam[f].nacc = L->out.tx.acc_n[i];
            memcpy(L->fam[f].sig, L->out.tx.sig, STATE_SIG_LEN);
            L->fam[f].n = 1;
            L->n_fam++;
        }
    }
}

static int
pick_killer_ix(const live_t *L, uint16_t *out_i)
{
    uint16_t i, first_pump = 0xffffu;

    for (i = 0; i < L->out.tx.n_ix; i++) {
        const ordered_ix_t *ix = &L->out.tx.ix[i];

        if (ix->proto != PROTO_PUMP) {
            continue;
        }
        if (first_pump == 0xffffu) {
            first_pump = i;
        }
        if (!is_pump_fast(ix->kind)) {
            *out_i = i;
            return 0;
        }
    }
    if (first_pump != 0xffffu) {
        *out_i = first_pump;
        return 0;
    }
    return -1;
}

static void
note_first_dirty(live_t *L, int rc, uint64_t slot)
{
    uint32_t p;
    uint16_t ki = 0;
    int have_ix;

    if (L->dirty_log == NULL || L->st == NULL) {
        return;
    }
    have_ix = pick_killer_ix(L, &ki);
    for (p = 0; p < L->st->n_pool; p++) {
        const state_pool_t *row;
        const tx_ix_t *vx;
        uint8_t nacc = 0;
        uint16_t dlen = 0;
        const uint8_t *disc = NULL;
        const uint8_t *prog = NULL;

        if (L->dirty_logged[p]) {
            continue;
        }
        if (L->pre_grade[p] != POOL_GRADE_EXACT) {
            continue;
        }
        if (compact_pool_get(L->st, p, &row) != 0
            || row->grade != POOL_GRADE_DIRTY) {
            continue;
        }
        L->dirty_logged[p] = 1;
        L->n_first_dirty++;
        if (L->pool_kill_rc[p] == 0 && rc >= 0 && rc < 9) {
            L->pool_kill_rc[p] = (uint8_t)rc;
        }
        if (have_ix == 0 && ki < L->out.view.n_ix) {
            vx = &L->out.view.ix[ki];
            disc = vx->data;
            dlen = vx->dlen;
            nacc = L->out.tx.acc_n[ki];
            if (vx->prog < L->out.view.n_keys) {
                prog = L->out.view.key[vx->prog];
            }
        }
        fprintf(L->dirty_log,
                "{\"pool_hex\":\"");
        hex_write(L->dirty_log, row->pubkey, 32);
        fprintf(L->dirty_log, "\",\"slot\":%" PRIu64 ",\"sig_hex\":\"", slot);
        hex_write(L->dirty_log, L->out.tx.sig, STATE_SIG_LEN);
        fprintf(L->dirty_log,
                "\",\"cause\":\"%s\",\"apply_code\":%d,\"dep_class\":%u"
                ",\"dlen\":%u,\"nacc\":%u,\"where\":\"outer\""
                ",\"ix_index\":%u,\"account_index\":%u,\"writable\":%u"
                ",\"compact_row_present\":%u,\"compact_row_exact\":%u"
                ",\"required_by\":\"%s\""
                ",\"disc_hex\":\"",
                first_dirty_cause(rc, &L->out.ar),
                L->out.ar.code, (unsigned)L->out.ar.dep_class,
                (unsigned)dlen, (unsigned)nacc,
                (unsigned)(L->out.ar.have_off ? L->out.ar.off.ix_index : ki),
                (unsigned)L->out.ar.off.account_index,
                (unsigned)L->out.ar.off.writable,
                (unsigned)L->out.ar.off.compact_row_present,
                (unsigned)L->out.ar.off.compact_row_exact,
                L->out.ar.have_off ? deps_need_name(L->out.ar.off.required_by)
                                   : "none");
        if (disc != NULL && dlen >= 8u) {
            hex_write(L->dirty_log, disc, 8);
        }
        fprintf(L->dirty_log, "\",\"prog_hex\":\"");
        if (L->out.ar.have_off) {
            hex_write(L->dirty_log, L->out.ar.off.program, 32);
        } else if (prog != NULL) {
            hex_write(L->dirty_log, prog, 32);
        }
        fprintf(L->dirty_log, "\",\"account_pubkey\":\"");
        if (L->out.ar.have_off) {
            hex_write(L->dirty_log, L->out.ar.off.account, 32);
        }
        fprintf(L->dirty_log, "\"}\n");
        fflush(L->dirty_log);
    }
}

void
live_write_fam(const live_t *L)
{
    FILE *fp;
    uint32_t i;

    if (L == NULL || L->fam_path == NULL) {
        return;
    }
    fp = fopen(L->fam_path, "w");
    if (fp == NULL) {
        return;
    }
    for (i = 0; i < L->n_fam; i++) {
        const live_fam_t *f = &L->fam[i];

        fprintf(fp, "{\"n\":%" PRIu64 ",\"dlen\":%u,\"nacc\":%u,\"disc_hex\":\"",
                f->n, (unsigned)f->dlen, (unsigned)f->nacc);
        hex_write(fp, f->disc, 8);
        fprintf(fp, "\",\"sig_hex\":\"");
        hex_write(fp, f->sig, STATE_SIG_LEN);
        fprintf(fp, "\"}\n");
    }
    fclose(fp);
}

void
live_lut_poll(live_t *L)
{
    if (L == NULL || L->lut == NULL || L->lut->sealed) {
        return;
    }
    if (L->lut_learned_path != NULL) {
        (void)lut_cache_load_jsonl(L->lut, L->lut_learned_path);
        lut_cache_prune_miss(L->lut);
    }
    if (L->lut_miss_log != NULL) {
        (void)lut_cache_flush_miss(L->lut, L->lut_miss_log);
    }
}

void
live_auth_poll(live_t *L)
{
    if (L == NULL || L->av == NULL || L->auth_path == NULL) {
        return;
    }
    (void)authvec_load_jsonl_from(L->av, L->auth_path, &L->auth_off);
    live_judge(L);
}

int
live_open_logs(live_t *L, const char *pred_path, const char *mm_path)
{
    if (L == NULL) {
        return -1;
    }
    if (pred_path != NULL) {
        L->pred_log = fopen(pred_path, "w");
        if (L->pred_log == NULL) {
            return -1;
        }
    }
    if (mm_path != NULL) {
        L->mismatch_log = fopen(mm_path, "w");
        if (L->mismatch_log == NULL) {
            return -1;
        }
    }
    return 0;
}

void
live_close_logs(live_t *L)
{
    if (L == NULL) {
        return;
    }
    if (L->pred_log != NULL) {
        fclose(L->pred_log);
        L->pred_log = NULL;
    }
    if (L->mismatch_log != NULL) {
        fclose(L->mismatch_log);
        L->mismatch_log = NULL;
    }
    if (L->lut_miss_log != NULL) {
        fclose(L->lut_miss_log);
        L->lut_miss_log = NULL;
    }
    if (L->disc_log != NULL) {
        fclose(L->disc_log);
        L->disc_log = NULL;
    }
    if (L->dirty_log != NULL) {
        fclose(L->dirty_log);
        L->dirty_log = NULL;
    }
    if (L->ps_log != NULL) {
        fclose(L->ps_log);
        L->ps_log = NULL;
    }
    if (L->walk_log != NULL) {
        fclose(L->walk_log);
        L->walk_log = NULL;
    }
}

static const char *
proto_name(uint8_t p)
{
    switch (p) {
    case PROTO_PUMP:
        return "PUMP";
    case PROTO_TOKEN:
        return "TOKEN";
    case PROTO_ATA:
        return "ATA";
    case PROTO_SYSTEM:
        return "SYSTEM";
    case PROTO_DLMM:
        return "DLMM";
    case PROTO_NONE:
        return "NONE";
    default:
        return "UNKNOWN";
    }
}

static const char *
kind_name(uint8_t k)
{
    switch (k) {
    case IX_KIND_PUMP_SELL:
        return "PUMP_SELL";
    case IX_KIND_PUMP_BUY_EQ:
        return "PUMP_BUY_EQ";
    case IX_KIND_PUMP_BUY:
        return "PUMP_BUY";
    case IX_KIND_TOKEN_XFER:
        return "TOKEN_XFER";
    case IX_KIND_TOKEN_SYNC:
        return "TOKEN_SYNC";
    case IX_KIND_TOKEN_CLOSE:
        return "TOKEN_CLOSE";
    case IX_KIND_ATA_CREATE:
        return "ATA_CREATE";
    case IX_KIND_ATA_INIT:
        return "ATA_INIT";
    case IX_KIND_SYS_TRANSFER:
        return "SYS_TRANSFER";
    default:
        return "OTHER";
    }
}

static const char *
pump_role(uint8_t a)
{
    static const char *r[] = {
        "pool", "user", "global_config", "base_mint", "quote_mint",
        "user_base_ata", "user_quote_ata", "pool_base_vault",
        "pool_quote_vault", "protocol_fee_recipient",
        "protocol_fee_ata", "base_token_program", "quote_token_program",
        "system_program", "ata_program", "event_authority"
    };

    if (a < (uint8_t)(sizeof(r) / sizeof(r[0]))) {
        return r[a];
    }
    return "other";
}

static int
key_writable(const ordered_tx_t *tx, uint8_t idx)
{
    uint16_t nws, first_ro_u;

    if (tx == NULL || idx >= tx->n_keys || !tx->hdr_ok) {
        return -1;
    }
    if (idx < tx->n_static) {
        if (idx < tx->nsig) {
            nws = (uint16_t)(tx->nsig - tx->nro_signed);
            return idx < nws;
        }
        first_ro_u = (uint16_t)(tx->n_static - tx->nro_unsigned);
        return idx < first_ro_u;
    }
    return (uint16_t)(idx - tx->n_static) < tx->n_lut_w;
}

static void
dump_token_id(FILE *fp, const compact_state_t *st, const char *name,
              uint32_t id)
{
    const state_token_t *t;

    fprintf(fp, "    %s id=", name);
    if (id == STATE_ACCT_NONE) {
        fprintf(fp, "NONE\n");
        return;
    }
    fprintf(fp, "%" PRIu32, id);
    if (compact_token_get(st, id, &t) != 0) {
        fprintf(fp, " lookup=MISS\n");
        return;
    }
    fprintf(fp, " live=%u ext=%u amount=%" PRIu64 " pk=",
            (unsigned)t->live, (unsigned)t->extensions_mask, t->amount);
    hex_write(fp, t->pubkey, 32);
    fputc('\n', fp);
}

static void
dump_trace_tx(live_t *L, int rc, uint64_t slot)
{
    const ordered_tx_t *tx;
    uint16_t i, k;
    uint8_t a;

    if (L == NULL || L->st == NULL) {
        return;
    }
    tx = &L->out.tx;
    fprintf(stderr, "\n=== TRACE sig=");
    hex_write(stderr, L->out.view.sig, STATE_SIG_LEN);
    fprintf(stderr,
            " slot=%" PRIu64 " pumpstate=%s lut_resolved=%u n_pump=%u "
            "n_ix=%u n_keys=%u n_static=%u n_alt=%u hdr_ok=%u\n",
            slot, pumpstate_name(rc), (unsigned)L->out.lut_resolved,
            (unsigned)L->out.n_pump, (unsigned)tx->n_ix, (unsigned)tx->n_keys,
            (unsigned)tx->n_static, (unsigned)L->out.view.n_alt,
            (unsigned)tx->hdr_ok);
    fprintf(stderr, "apply code=%d dep=%s required_by=%s ix=%u acc=%u w=%u "
            "row_present=%u row_exact=%u offend_pk=",
            L->out.ar.code, deps_class_name(L->out.ar.dep_class),
            deps_need_name(L->out.ar.off.required_by),
            (unsigned)L->out.ar.off.ix_index,
            (unsigned)L->out.ar.off.account_index,
            (unsigned)L->out.ar.off.writable,
            (unsigned)L->out.ar.off.compact_row_present,
            (unsigned)L->out.ar.off.compact_row_exact);
    hex_write(stderr, L->out.ar.off.account, 32);
    fprintf(stderr, "\noverlay_status=%u n_pool=%u n_token=%u n_sys=%u "
            "fail_step=%s fail_ix=%u\n",
            (unsigned)L->out.ar.overlay.status,
            (unsigned)L->out.ar.overlay.n,
            (unsigned)L->out.ar.overlay.n_token,
            (unsigned)L->out.ar.overlay.n_sys,
            apply_step_name(L->out.ar.fail_step),
            (unsigned)L->out.ar.fail_ix);
    if (L->out.ar.overlay.n > 0) {
        const overlay_row_t *r = &L->out.ar.overlay.row[0];

        fprintf(stderr,
                "overlay_pool0 id=%" PRIu32 " dirty=%u rb=%" PRIu64
                " rq=%" PRIu64 "\n",
                r->pool_id, (unsigned)r->dirty, r->pump.reserve_base,
                r->pump.reserve_quote);
    }
    fprintf(stderr, "keys:\n");
    for (k = 0; k < tx->n_keys && k < STATE_KEY_MAX; k++) {
        int w = key_writable(tx, (uint8_t)k);
        uint32_t pid = 0, tid = 0;

        fprintf(stderr, "  [%u] %s w=%d pk=", (unsigned)k,
                k < tx->n_static ? "static" : "lut", w);
        hex_write(stderr, tx->key[k], 32);
        if (compact_pool_find(L->st, tx->key[k], &pid) == 0) {
            fprintf(stderr, " POOL id=%" PRIu32 " grade=%u", pid,
                    (unsigned)L->st->pool[pid].grade);
        }
        if (compact_token_find(L->st, tx->key[k], &tid) == 0) {
            fprintf(stderr, " TOKEN id=%" PRIu32, tid);
        }
        fputc('\n', stderr);
    }
    for (i = 0; i < tx->n_ix; i++) {
        const ordered_ix_t *ix = &tx->ix[i];

        fprintf(stderr,
                "ix[%u] proto=%s kind=%s class=%s relevant=%u nacc=%u "
                "prog=%u ain=%" PRIu64 " min_out=%" PRIu64 " dir=%u\n",
                (unsigned)i, proto_name(ix->proto), kind_name(ix->kind),
                deps_class_name(L->out.ar.ix_class[i]),
                (unsigned)ix->relevant, (unsigned)tx->acc_n[i],
                (unsigned)ix->prog, ix->amount_in, ix->min_out,
                (unsigned)ix->direction);
        for (a = 0; a < tx->acc_n[i]; a++) {
            uint8_t idx = tx->acc_ix[i][a];

            fprintf(stderr, "    acc[%u] key[%u] role=%s\n", (unsigned)a,
                    (unsigned)idx, ix->proto == PROTO_PUMP ? pump_role(a)
                                                          : "ix_acc");
        }
        fprintf(stderr, "    pool_id=");
        if (ix->pool_id == STATE_ACCT_NONE) {
            fprintf(stderr, "NONE\n");
        } else {
            fprintf(stderr, "%" PRIu32 "\n", ix->pool_id);
        }
        if (ix->pool_id != STATE_ACCT_NONE && ix->pool_id < L->st->n_pool) {
            const state_pool_t *p = &L->st->pool[ix->pool_id];

            fprintf(stderr, "      pool_pk=");
            hex_write(stderr, p->pubkey, 32);
            fprintf(stderr,
                    " auth=%u grade=%u anchor=%" PRIu64
                    " rb=%" PRIu64 " rq=%" PRIu64 " vq=%" PRId64
                    " lp=%" PRIu64 " proto_bps=%" PRIu64 " creator_bps=%" PRIu64
                    " disabled=%u status=%u\n",
                    (unsigned)p->auth_bits, (unsigned)p->grade, p->anchor_slot,
                    p->pump.reserve_base, p->pump.reserve_quote,
                    p->pump.virtual_quote, p->pump.lp_fee_bps,
                    p->pump.protocol_fee_bps, p->pump.creator_fee_bps,
                    (unsigned)p->pump.disabled, (unsigned)p->pump.status);
            fprintf(stderr, "      vault_base=");
            hex_write(stderr, p->vault_base, 32);
            fprintf(stderr, " vault_quote=");
            hex_write(stderr, p->vault_quote, 32);
            fputc('\n', stderr);
            if (is_pump_fast(ix->kind)) {
                pump_quote_t q;

                if (pump_quote_exact_in(&p->pump, ix->amount_in, ix->direction,
                                        &q) == 0 && q.valid) {
                    fprintf(stderr,
                            "      kernel_out=%" PRIu64 " min_out=%" PRIu64
                            " slip=%u\n",
                            q.amount_out, ix->min_out,
                            (unsigned)(q.amount_out < ix->min_out));
                } else {
                    fprintf(stderr, "      kernel_out=FAIL\n");
                }
            }
        }
        dump_token_id(stderr, L->st, "src_token", ix->src_token);
        dump_token_id(stderr, L->st, "dst_token", ix->dst_token);
        dump_token_id(stderr, L->st, "vault_base", ix->vault_base);
        dump_token_id(stderr, L->st, "vault_quote", ix->vault_quote);
        dump_token_id(stderr, L->st, "fee_proto", ix->fee_proto);
        dump_token_id(stderr, L->st, "fee_creator", ix->fee_creator);
        fprintf(stderr, "    src_sys=");
        if (ix->src_sys == STATE_ACCT_NONE) {
            fprintf(stderr, "NONE\n");
        } else {
            fprintf(stderr, "%" PRIu32 "\n", ix->src_sys);
        }
        fprintf(stderr, "    dst_sys=");
        if (ix->dst_sys == STATE_ACCT_NONE) {
            fprintf(stderr, "NONE\n");
        } else {
            fprintf(stderr, "%" PRIu32 "\n", ix->dst_sys);
        }
    }
    L->trace_stop = 1;
}

static int
walk_eq32(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 32) == 0;
}

static uint64_t
walk_token_amt(const compact_state_t *st, const uint8_t pk[32])
{
    uint32_t id;
    const state_token_t *t;

    if (compact_token_find(st, pk, &id) != 0
        || compact_token_get(st, id, &t) != 0) {
        return 0;
    }
    return t->amount;
}

static void
log_walk_tx(live_t *L, int rc, uint64_t slot)
{
    const tx_view_t *v;
    uint16_t i;
    uint32_t pid;
    int lut_miss, touch_pool, touch_vb, touch_vq, static_pool, ours;
    int pump_kind;
    uint8_t gpre, gpost;

    if (L == NULL || !L->have_walk || L->walk_log == NULL) {
        return;
    }
    if (slot <= L->walk_from || slot > L->walk_to) {
        return;
    }
    pid = L->walk_pool_id;
    if (pid == STATE_ACCT_NONE) {
        if (compact_pool_find(L->st, L->walk_pk, &pid) == 0) {
            L->walk_pool_id = pid;
            memcpy(L->walk_vb, L->st->pool[pid].vault_base, 32);
            memcpy(L->walk_vq, L->st->pool[pid].vault_quote, 32);
        }
    } else {
        pid = L->walk_pool_id;
    }
    v = &L->out.view;
    lut_miss = (rc == PS_LUT);
    touch_pool = 0;
    touch_vb = 0;
    touch_vq = 0;
    static_pool = 0;
    for (i = 0; i < v->n_keys; i++) {
        if (walk_eq32(v->key[i], L->walk_pk)) {
            touch_pool = 1;
        }
        if (walk_eq32(v->key[i], L->walk_vb)) {
            touch_vb = 1;
        }
        if (walk_eq32(v->key[i], L->walk_vq)) {
            touch_vq = 1;
        }
    }
    for (i = 0; i < v->n_static; i++) {
        if (walk_eq32(v->key[i], L->walk_pk)) {
            static_pool = 1;
        }
    }
    ours = 0;
    pump_kind = -1;
    for (i = 0; i < L->out.tx.n_ix; i++) {
        if (L->out.tx.ix[i].proto != PROTO_PUMP) {
            continue;
        }
        if (pump_kind < 0) {
            pump_kind = (int)L->out.tx.ix[i].kind;
        }
        if (pid != STATE_ACCT_NONE && L->out.tx.ix[i].pool_id == pid) {
            ours = 1;
        }
    }
    if (!lut_miss && !touch_pool && !touch_vb && !touch_vq && !ours) {
        return;
    }
    L->n_walk++;
    if (lut_miss) {
        L->n_walk_lut++;
    }
    if (touch_pool || touch_vb || touch_vq || ours) {
        L->n_walk_touch++;
    }
    gpre = 0;
    gpost = 0;
    if (pid != STATE_ACCT_NONE && pid < STATE_POOL_MAX) {
        gpre = L->pre_grade[pid];
        gpost = L->st->pool[pid].grade;
    }
    fprintf(L->walk_log,
            "{\"slot\":%" PRIu64 ",\"rc\":\"%s\",\"lut_resolved\":%u"
            ",\"lut_miss\":%u,\"n_alt\":%u,\"n_static\":%u,\"n_keys\":%u"
            ",\"n_ix\":%u,\"n_pump\":%u,\"apply_code\":%d,\"dep_class\":%u"
            ",\"pump_kind\":%d,\"touch_pool\":%u,\"touch_vault_base\":%u"
            ",\"touch_vault_quote\":%u,\"static_pool\":%u,\"ours\":%u"
            ",\"grade_pre\":%u,\"grade_post\":%u",
            slot, pumpstate_name(rc), (unsigned)L->out.lut_resolved,
            (unsigned)lut_miss, (unsigned)v->n_alt, (unsigned)v->n_static,
            (unsigned)v->n_keys, (unsigned)L->out.tx.n_ix,
            (unsigned)L->out.n_pump, L->out.ar.code,
            (unsigned)L->out.ar.dep_class, pump_kind,
            (unsigned)touch_pool, (unsigned)touch_vb, (unsigned)touch_vq,
            (unsigned)static_pool, (unsigned)ours, (unsigned)gpre,
            (unsigned)gpost);
    if (pid != STATE_ACCT_NONE && pid < L->st->n_pool) {
        const pump_state_t *pre = &L->pre_snap[pid];
        const pump_state_t *post = &L->st->pool[pid].pump;

        fprintf(L->walk_log,
                ",\"rb_pre\":%" PRIu64 ",\"rq_pre\":%" PRIu64
                ",\"rb_post\":%" PRIu64 ",\"rq_post\":%" PRIu64
                ",\"vb_amt_pre\":%" PRIu64 ",\"vq_amt_pre\":%" PRIu64
                ",\"vb_amt_post\":%" PRIu64 ",\"vq_amt_post\":%" PRIu64
                ",\"vq_eff\":%" PRId64,
                pre->reserve_base, pre->reserve_quote, post->reserve_base,
                post->reserve_quote, L->walk_vb_pre, L->walk_vq_pre,
                walk_token_amt(L->st, L->walk_vb),
                walk_token_amt(L->st, L->walk_vq), post->virtual_quote);
    }
    fprintf(L->walk_log, ",\"sig_hex\":\"");
    hex_write(L->walk_log, v->sig, STATE_SIG_LEN);
    if (v->n_alt != 0) {
        fprintf(L->walk_log, "\",\"alt0\":\"");
        hex_write(L->walk_log, v->alt[0].table, 32);
    }
    fprintf(L->walk_log, "\"}\n");
    fflush(L->walk_log);
}

int
live_on_framed(live_t *L, const uint8_t *bytes, uint32_t len, uint64_t slot,
               uint64_t of_rx_tsc)
{
    int rc;
    uint16_t i;
    uint32_t p;
    uint64_t ready;

    if (L == NULL || L->st == NULL || bytes == NULL || len == 0) {
        return -1;
    }
    if (L->max_framed != 0 && L->n_framed >= L->max_framed) {
        return 0;
    }
    L->n_framed++;
    if (L->have_walk && L->walk_pool_id == STATE_ACCT_NONE) {
        uint32_t wid;

        if (compact_pool_find(L->st, L->walk_pk, &wid) == 0) {
            L->walk_pool_id = wid;
            memcpy(L->walk_vb, L->st->pool[wid].vault_base, 32);
            memcpy(L->walk_vq, L->st->pool[wid].vault_quote, 32);
        }
    }
    if (L->have_walk) {
        L->walk_vb_pre = walk_token_amt(L->st, L->walk_vb);
        L->walk_vq_pre = walk_token_amt(L->st, L->walk_vq);
    }
    for (p = 0; p < L->st->n_pool; p++) {
        L->pre_snap[p] = L->st->pool[p].pump;
        L->pre_grade[p] = L->st->pool[p].grade;
        L->pre_auth[p] = L->st->pool[p].auth_bits;
        L->pre_anchor[p] = L->st->pool[p].anchor_slot;
    }
    rc = (L->lut != NULL)
             ? pumpstate_from_cache(L->st, L->lut, bytes, len, slot, &L->out)
             : pumpstate_from_bytes(L->st, NULL, bytes, len, slot, &L->out);
    if (L->have_trace
        && memcmp(L->out.view.sig, L->trace_sig, STATE_SIG_LEN) == 0) {
        dump_trace_tx(L, rc, slot);
    }
    log_walk_tx(L, rc, slot);
    ready = rdtsc_now();
    if (rc >= 0 && (unsigned)rc < 9u) {
        L->reason[rc]++;
    }
    if (L->out.n_pump != 0) {
        L->n_pump++;
        if (rc == PS_OK || rc == PS_APPLY || rc == PS_UNSUPPORTED
            || rc == PS_GAP || rc == PS_NO_PRESTATE || rc == PS_LUT) {
            count_supported(L);
        }
    }
    keep_tx(L, bytes, len, slot);
    note_disc(L);
    note_fam(L);
    note_first_dirty(L, rc, slot);
    log_ps_event(L, rc, slot);
    if (rc == PS_OK) {
        L->n_applied++;
        if (L->out.ar.n_no_quote_ix != 0) {
            L->n_no_quote_tx++;
        }
        for (i = 0; i < L->out.ar.overlay.n; i++) {
            const overlay_row_t *r = &L->out.ar.overlay.row[i];
            const pump_state_t *after;
            const pump_state_t *pr = NULL;

            if (!r->dirty) {
                continue;
            }
            if (overlay_pump(&L->out.ar.overlay, r->pool_id, &after) != 0) {
                continue;
            }
            if (r->pool_id < L->st->n_pool) {
                pr = &L->pre_snap[r->pool_id];
            }
            if (r->pool_id < STATE_POOL_MAX && !L->saw_apply[r->pool_id]) {
                L->saw_apply[r->pool_id] = 1;
                L->n_caught_up++;
            }
            if (authvec_note_pred_full(L->av, L->st, &L->out.tx, r->pool_id,
                                       pr, after, of_rx_tsc, ready) != 0) {
                L->n_pred_drop++;
            } else {
                log_pred(L, r->pool_id, after, of_rx_tsc, ready);
            }
        }
        if (L->nlat < LIVE_LAT_MAX && ready >= of_rx_tsc && of_rx_tsc != 0) {
            L->lat[L->nlat++] = ready - of_rx_tsc;
        }
    }
    return rc;
}

void
live_judge(live_t *L)
{
    uint32_t i;

    if (L == NULL || L->av == NULL || L->st == NULL) {
        return;
    }
    authvec_judge(L->av);
    for (i = 0; i < L->av->n_pred; i++) {
        auth_pred_t *p = &L->av->pred[i];

        if (p->judged != AUTH_MISMATCH) {
            continue;
        }
        (void)compact_pool_mark_dirty(L->st, p->pool_id);
        dump_mismatch(L, p);
        L->n_mismatch_dirty++;
    }
}

uint32_t
live_n_exact_pools(const compact_state_t *st)
{
    uint32_t i, n = 0;

    if (st == NULL) {
        return 0;
    }
    for (i = 0; i < st->n_pool; i++) {
        if (st->pool[i].live && st->pool[i].grade == POOL_GRADE_EXACT) {
            n++;
        }
    }
    return n;
}

uint32_t
live_n_dirty_pools(const compact_state_t *st)
{
    uint32_t i, n = 0;

    if (st == NULL) {
        return 0;
    }
    for (i = 0; i < st->n_pool; i++) {
        if (st->pool[i].live && st->pool[i].grade == POOL_GRADE_DIRTY) {
            n++;
        }
    }
    return n;
}

static int
cmpu64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

void
live_print(const live_t *L, FILE *fp)
{
    uint64_t match, mis, inc, verifiable, rel;
    double correctness = 0, coverage = 0, exact_rate = 0;
    uint32_t n_disc, n_dirty;

    if (L == NULL || fp == NULL || L->st == NULL || L->av == NULL) {
        return;
    }
    match = L->av->n_exact;
    mis = L->av->n_mismatch;
    inc = L->av->n_incomplete;
    verifiable = match + mis;
    rel = L->n_pump;
    if (verifiable != 0) {
        correctness = 100.0 * (double)match / (double)verifiable;
        exact_rate = correctness;
    }
    if (rel != 0) {
        coverage = 100.0 * (double)L->n_applied / (double)rel;
    }
    n_disc = L->st->n_pool;
    n_dirty = live_n_dirty_pools(L->st);
    {
        uint32_t n_untouched = 0, n_advanced = 0, pi;

        for (pi = 0; pi < L->st->n_pool; pi++) {
            if (L->st->pool[pi].grade != POOL_GRADE_EXACT) {
                continue;
            }
            if (L->saw_apply[pi]) {
                n_advanced++;
            } else {
                n_untouched++;
            }
        }
        fprintf(fp, "LIVE-STATE-001  (shadow; AUTH does not drive state)\n");
        fprintf(fp, "Pump pools discovered:       %10" PRIu32 "\n", n_disc);
        fprintf(fp, "EXACT_UNTOUCHED:             %10" PRIu32 "\n",
                n_untouched);
        fprintf(fp, "EXACT_ADVANCED:              %10" PRIu32 "\n",
                n_advanced);
        fprintf(fp, "DIRTY:                       %10" PRIu32 "\n", n_dirty);
        fprintf(fp, "Caught up / LIVE:            %10" PRIu64 "\n",
                L->n_caught_up);
    }
    fprintf(fp, "First DIRTY events:          %10" PRIu32 "\n",
            L->n_first_dirty);
    fprintf(fp, "Pump ix families:            %10" PRIu32 "\n", L->n_fam);
    fprintf(fp, "\n");
    fprintf(fp, "OF framed tx:                %10" PRIu64 "\n", L->n_framed);
    fprintf(fp, "Pump tx:                     %10" PRIu64 "\n", L->n_pump);
    fprintf(fp, "Supported:                   %10" PRIu64 "\n", L->n_supported);
    fprintf(fp, "Applied:                     %10" PRIu64 "\n", L->n_applied);
    fprintf(fp, "NO_QUOTE extra (applied):    %10" PRIu64 "\n",
            L->n_no_quote_tx);
    fprintf(fp, "\n");
    fprintf(fp, "AUTH verifiable:             %10" PRIu64 "\n", verifiable);
    fprintf(fp, "MATCH:                       %10" PRIu64 "\n", match);
    fprintf(fp, "MISMATCH:                    %10" PRIu64 "\n", mis);
    fprintf(fp, "AUTH_INCOMPLETE:             %10" PRIu64 "\n", inc);
    fprintf(fp, "NO_PRESTATE:                 %10" PRIu64 "\n",
            L->reason[PS_NO_PRESTATE]);
    fprintf(fp, "LUT_MISS:                    %10" PRIu64 "\n",
            L->reason[PS_LUT]);
    if (L->have_walk) {
        fprintf(fp, "WALK rows:                   %10" PRIu64 "\n", L->n_walk);
        fprintf(fp, "WALK lut_miss:               %10" PRIu64 "\n",
                L->n_walk_lut);
        fprintf(fp, "WALK resolved-touch:         %10" PRIu64 "\n",
                L->n_walk_touch);
    }
    fprintf(fp, "UNSUPPORTED:                 %10" PRIu64 "\n",
            L->reason[PS_UNSUPPORTED]);
    fprintf(fp, "GAP:                         %10" PRIu64 "\n",
            L->reason[PS_GAP]);
    fprintf(fp, "BEFORE_ANCHOR:               %10" PRIu64 "\n",
            L->reason[PS_BEFORE_ANCHOR]);
    fprintf(fp, "pred_drop:                   %10" PRIu64 "\n", L->n_pred_drop);
    if (L->lut != NULL) {
        uint32_t nfull = (L->lut->full != NULL) ? L->lut->full->n : 0;

        fprintf(fp, "\nLUT cache gen=%" PRIu64 " full_tables=%u sparse=%u "
                "pending_miss=%u quar=%u install=%" PRIu64 " learn=%" PRIu64
                "\n  lookup_ok=%" PRIu64 " lookup_fail=%" PRIu64 "\n",
                L->lut->generation, nfull, L->lut->n_sp, L->lut->n_miss,
                L->lut->n_quar, L->lut->n_install, L->lut->n_learn,
                L->lut->n_lookup_ok, L->lut->n_lookup_fail);
    }
    fprintf(fp, "\n");
    if (verifiable == 0) {
        fprintf(fp, "Exact rate:                  n/a (no AUTH vector)\n");
    } else {
        fprintf(fp, "Exact rate:                  %10.4f%%  "
                "(MATCH/(MATCH+MISMATCH); incomplete excluded)\n",
                exact_rate);
    }
    fprintf(fp, "Correctness:                 %10.4f%%\n", correctness);
    fprintf(fp, "Coverage:                    %10.4f%%  "
            "(applied / Pump tx)\n", coverage);
    if (L->nlat > 0) {
        uint64_t tmp[LIVE_LAT_MAX];
        uint32_t last = L->nlat - 1u;
        double hz = 3.5e9;

        memcpy(tmp, L->lat, L->nlat * sizeof(tmp[0]));
        qsort(tmp, L->nlat, sizeof(tmp[0]), cmpu64);
        fprintf(fp, "\nrx → state-ready (tsc, hz=%.3e assumed):\n", hz);
        fprintf(fp, "p50  %10.1f us\n",
                (double)tmp[L->nlat / 2u] * 1e6 / hz);
        fprintf(fp, "p95  %10.1f us\n",
                (double)tmp[(uint32_t)(0.95 * (double)last)] * 1e6 / hz);
        fprintf(fp, "p99  %10.1f us\n",
                (double)tmp[(uint32_t)(0.99 * (double)last)] * 1e6 / hz);
    }
}
