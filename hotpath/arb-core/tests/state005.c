/*
 * STATE-005 — frozen dlmm_apply_swap on a real before/N/after triple.
 * Control plane writes .tri; this only applies and compares. Kernel frozen.
 */
#include "dlmm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int
load_state(FILE *f, dlmm_state_t *s)
{
    unsigned bin_step, status, bf, fp, dp, rf, pshare, bfpw, cfm;
    unsigned vacc, vref, nbin;
    int iref;
    uint16_t i;

    memset(s, 0, sizeof(*s));
    if (fscanf(f,
               "%d %u %u %u %u %u %u %u %u %u %u %u %u %u %d %" SCNd64
               " %" SCNu64 " %" SCNu64 " %u",
               &s->active_id, &bin_step, &status, &bf, &fp, &dp, &rf,
               &s->parameters.variable_fee_control,
               &s->parameters.max_volatility_accumulator,
               &pshare, &bfpw, &cfm, &vacc, &vref, &iref,
               &s->v_parameters.last_update_timestamp,
               &s->reserve_x, &s->reserve_y, &nbin) != 19) {
        return -1;
    }
    s->bin_step = (uint16_t)bin_step;
    s->status = (uint8_t)status;
    s->parameters.base_factor = (uint16_t)bf;
    s->parameters.filter_period = (uint16_t)fp;
    s->parameters.decay_period = (uint16_t)dp;
    s->parameters.reduction_factor = (uint16_t)rf;
    s->parameters.protocol_share = (uint16_t)pshare;
    s->parameters.base_fee_power_factor = (uint8_t)bfpw;
    s->parameters.collect_fee_mode = (uint8_t)cfm;
    s->v_parameters.volatility_accumulator = vacc;
    s->v_parameters.volatility_reference = vref;
    s->v_parameters.index_reference = iref;
    if (nbin > DLMM_BINS_MAX) {
        return -1;
    }
    s->nbin = (uint16_t)nbin;
    for (i = 0; i < s->nbin; i++) {
        if (fscanf(f, "%d %" SCNu64 " %" SCNu64,
                   &s->bins[i].id, &s->bins[i].amount_x,
                   &s->bins[i].amount_y) != 3) {
            return -1;
        }
    }
    return 0;
}

static const dlmm_bin_t *
find_bin(const dlmm_state_t *s, int32_t id)
{
    uint16_t i;
    for (i = 0; i < s->nbin; i++) {
        if (s->bins[i].id == id) {
            return &s->bins[i];
        }
    }
    return NULL;
}

int
main(int argc, char **argv)
{
    FILE *f;
    dlmm_state_t before, chain, got;
    dlmm_swap_ix_t ix;
    dlmm_apply_result_t res;
    uint64_t ev_out, ev_fee, ev_proto;
    int32_t ev_start, ev_end;
    int64_t now_ts;
    uint32_t s4y;
    char tag[16];
    int exact = 1;
    uint16_t i;
    uint32_t n_bin_mis = 0;
    uint32_t n_touched = 0;
    int32_t lo, hi, id;

    if (argc != 2) {
        fprintf(stderr, "usage: %s triple.tri\n", argv[0]);
        return 1;
    }
    f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    if (fscanf(f, "%15s", tag) != 1 || strcmp(tag, "ST5") != 0) {
        fprintf(stderr, "bad magic\n");
        fclose(f);
        return 1;
    }
    if (fscanf(f, "%15s %u %" SCNu64 " %" SCNu64 " %" SCNd64,
               tag, &s4y, &ix.amount_in, &ix.min_amount_out, &now_ts) != 5
        || strcmp(tag, "N") != 0) {
        fprintf(stderr, "bad N\n");
        fclose(f);
        return 1;
    }
    ix.swap_for_y = (uint8_t)s4y;
    if (fscanf(f, "%15s", tag) != 1 || strcmp(tag, "BEFORE") != 0
        || load_state(f, &before) != 0) {
        fprintf(stderr, "bad BEFORE\n");
        fclose(f);
        return 1;
    }
    if (fscanf(f, "%15s", tag) != 1 || strcmp(tag, "AFTER") != 0
        || load_state(f, &chain) != 0) {
        fprintf(stderr, "bad AFTER\n");
        fclose(f);
        return 1;
    }
    if (fscanf(f, "%15s %" SCNu64 " %" SCNu64 " %" SCNu64 " %d %d",
               tag, &ev_out, &ev_fee, &ev_proto, &ev_start, &ev_end) != 6
        || strcmp(tag, "EVENT") != 0) {
        fprintf(stderr, "bad EVENT\n");
        fclose(f);
        return 1;
    }
    fclose(f);
    before.now_ts = now_ts;

    printf("STATE-005  ain=%" PRIu64 " s4y=%u now=%" PRId64 " bins=%u\n",
           ix.amount_in, (unsigned)ix.swap_for_y, now_ts, (unsigned)before.nbin);
    printf("  before active=%d vol=%u/%u idx=%d last=%" PRId64 "\n",
           before.active_id,
           before.v_parameters.volatility_accumulator,
           before.v_parameters.volatility_reference,
           before.v_parameters.index_reference,
           before.v_parameters.last_update_timestamp);
    printf("  chain  active=%d vol=%u/%u idx=%d last=%" PRId64 "\n",
           chain.active_id,
           chain.v_parameters.volatility_accumulator,
           chain.v_parameters.volatility_reference,
           chain.v_parameters.index_reference,
           chain.v_parameters.last_update_timestamp);
    printf("  event  %d->%d out=%" PRIu64 " fee=%" PRIu64 " proto=%" PRIu64 "\n",
           ev_start, ev_end, ev_out, ev_fee, ev_proto);

    if (dlmm_apply_swap(&before, &ix, &got, &res) != 0) {
        printf("APPLY_FAIL\n");
        return 2;
    }

    lo = ev_start < ev_end ? ev_start : ev_end;
    hi = ev_start < ev_end ? ev_end : ev_start;
    printf("  pred   active=%d vol=%u/%u idx=%d last=%" PRId64 "\n",
           got.active_id,
           got.v_parameters.volatility_accumulator,
           got.v_parameters.volatility_reference,
           got.v_parameters.index_reference,
           got.v_parameters.last_update_timestamp);
    printf("  pred   out=%" PRIu64 " fee=%" PRIu64 " proto=%" PRIu64 "\n",
           res.amount_out, res.fee, res.protocol_fee);

    if (got.active_id != chain.active_id) {
        printf("MIS active %d vs %d\n", got.active_id, chain.active_id);
        exact = 0;
    }
    if (got.active_id != ev_end) {
        printf("MIS active vs event_end %d vs %d\n", got.active_id, ev_end);
        exact = 0;
    }
    if (got.v_parameters.volatility_accumulator
        != chain.v_parameters.volatility_accumulator) {
        printf("MIS vol_acc %u vs %u\n",
               got.v_parameters.volatility_accumulator,
               chain.v_parameters.volatility_accumulator);
        exact = 0;
    }
    if (got.v_parameters.volatility_reference
        != chain.v_parameters.volatility_reference) {
        printf("MIS vol_ref %u vs %u\n",
               got.v_parameters.volatility_reference,
               chain.v_parameters.volatility_reference);
        exact = 0;
    }
    if (got.v_parameters.index_reference
        != chain.v_parameters.index_reference) {
        printf("MIS idx_ref %d vs %d\n",
               got.v_parameters.index_reference,
               chain.v_parameters.index_reference);
        exact = 0;
    }
    if (res.amount_out != ev_out) {
        printf("MIS amount_out %" PRIu64 " vs %" PRIu64 "\n",
               res.amount_out, ev_out);
        exact = 0;
    }
    if (res.fee != ev_fee) {
        printf("MIS fee %" PRIu64 " vs %" PRIu64 "\n", res.fee, ev_fee);
        exact = 0;
    }
    if (res.protocol_fee != ev_proto) {
        printf("MIS protocol_fee %" PRIu64 " vs %" PRIu64 "\n",
               res.protocol_fee, ev_proto);
        exact = 0;
    }
    if (got.v_parameters.last_update_timestamp
        != chain.v_parameters.last_update_timestamp) {
        printf("NOTE last_upd %" PRId64 " vs %" PRId64
               "  (kernel may not write this)\n",
               got.v_parameters.last_update_timestamp,
               chain.v_parameters.last_update_timestamp);
    }

    for (id = lo; id <= hi; id++) {
        const dlmm_bin_t *a = find_bin(&got, id);
        const dlmm_bin_t *b = find_bin(&chain, id);
        n_touched++;
        if (a == NULL || b == NULL) {
            printf("MIS bin %d missing pred=%d chain=%d\n",
                   id, a != NULL, b != NULL);
            n_bin_mis++;
            exact = 0;
            continue;
        }
        if (a->amount_x != b->amount_x || a->amount_y != b->amount_y) {
            printf("MIS bin %d  pred x=%" PRIu64 " y=%" PRIu64
                   "  chain x=%" PRIu64 " y=%" PRIu64 "\n",
                   id, a->amount_x, a->amount_y, b->amount_x, b->amount_y);
            n_bin_mis++;
            exact = 0;
        } else {
            printf("  bin %d  x=%" PRIu64 " y=%" PRIu64 "  exact\n",
                   id, a->amount_x, a->amount_y);
        }
    }
    for (i = 0; i < chain.nbin; i++) {
        const dlmm_bin_t *a;
        const dlmm_bin_t *pre;
        if (chain.bins[i].id >= lo && chain.bins[i].id <= hi) {
            continue;
        }
        a = find_bin(&got, chain.bins[i].id);
        pre = find_bin(&before, chain.bins[i].id);
        if (pre == NULL || a == NULL) {
            continue;
        }
        if (a->amount_x != chain.bins[i].amount_x
            || a->amount_y != chain.bins[i].amount_y) {
            printf("MIS untouched bin %d changed\n", chain.bins[i].id);
            n_bin_mis++;
            exact = 0;
        }
        if (a->amount_x != pre->amount_x || a->amount_y != pre->amount_y) {
            printf("MIS pred moved untouched bin %d\n", chain.bins[i].id);
            n_bin_mis++;
            exact = 0;
        }
    }

    printf("touched=%u bin_mis=%u  %s\n",
           n_touched, n_bin_mis, exact ? "EXACT" : "MISMATCH");
    return exact ? 0 : 3;
}
