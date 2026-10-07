#include "dlmm.h"
#include "pump.h"
#include "swapix.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * STATE-010-FASTSOAK — production swapix + apply. Not a Python kernel.
 * stdin:  nkeys u16 | keys n*32 | nacct u16 | acc[] u8 | dlen u16 | data
 *         blob_len u16 | blob (write_dlmm / write_pump)
 * stdout: one JSON object (s_prime) or {"error":"..."}
 */

static int
rd(const uint8_t **p, const uint8_t *end, void *dst, size_t n)
{
    if (*p + n > end) {
        return -1;
    }
    memcpy(dst, *p, n);
    *p += n;
    return 0;
}

static int
load_dlmm_blob(const uint8_t *p, uint16_t n, dlmm_state_t *s)
{
    const uint8_t *e = p + n;
    uint16_t i, nbin;

    memset(s, 0, sizeof(*s));
    if (rd(&p, e, &s->active_id, 4) != 0 || rd(&p, e, &s->bin_step, 2) != 0
        || rd(&p, e, &s->status, 1) != 0) {
        return -1;
    }
    if (rd(&p, e, &s->parameters.base_factor, 2) != 0
        || rd(&p, e, &s->parameters.filter_period, 2) != 0
        || rd(&p, e, &s->parameters.decay_period, 2) != 0
        || rd(&p, e, &s->parameters.reduction_factor, 2) != 0
        || rd(&p, e, &s->parameters.variable_fee_control, 4) != 0
        || rd(&p, e, &s->parameters.max_volatility_accumulator, 4) != 0
        || rd(&p, e, &s->parameters.protocol_share, 2) != 0
        || rd(&p, e, &s->parameters.base_fee_power_factor, 1) != 0
        || rd(&p, e, &s->parameters.collect_fee_mode, 1) != 0) {
        return -1;
    }
    if (rd(&p, e, &s->v_parameters.volatility_accumulator, 4) != 0
        || rd(&p, e, &s->v_parameters.volatility_reference, 4) != 0
        || rd(&p, e, &s->v_parameters.index_reference, 4) != 0
        || rd(&p, e, &s->v_parameters.last_update_timestamp, 8) != 0) {
        return -1;
    }
    if (rd(&p, e, &s->reserve_x, 8) != 0 || rd(&p, e, &s->reserve_y, 8) != 0
        || rd(&p, e, &s->now_ts, 8) != 0 || rd(&p, e, &nbin, 2) != 0) {
        return -1;
    }
    if (nbin > DLMM_BINS_MAX) {
        return -1;
    }
    s->nbin = nbin;
    for (i = 0; i < nbin; i++) {
        if (rd(&p, e, &s->bins[i].id, 4) != 0
            || rd(&p, e, &s->bins[i].amount_x, 8) != 0
            || rd(&p, e, &s->bins[i].amount_y, 8) != 0) {
            return -1;
        }
    }
    return 0;
}

static int
load_pump_blob(const uint8_t *p, uint16_t n, pump_state_t *s)
{
    const uint8_t *e = p + n;
    memset(s, 0, sizeof(*s));
    return rd(&p, e, &s->reserve_base, 8)
        || rd(&p, e, &s->reserve_quote, 8)
        || rd(&p, e, &s->virtual_quote, 8)
        || rd(&p, e, &s->lp_fee_bps, 8)
        || rd(&p, e, &s->protocol_fee_bps, 8)
        || rd(&p, e, &s->creator_fee_bps, 8)
        || rd(&p, e, &s->disabled, 1)
        || rd(&p, e, &s->status, 1);
}

int
main(void)
{
    uint8_t buf[1 << 16];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    const uint8_t *p = buf;
    const uint8_t *end = buf + n;
    uint16_t nkeys, nacct, dlen, blen;
    const uint8_t *keys;
    const uint8_t *acc;
    const uint8_t *data;
    const uint8_t *blob;
    swapix_t ix;
    uint16_t i;

    if (rd(&p, end, &nkeys, 2) != 0 || nkeys == 0 || nkeys > 256) {
        printf("{\"error\":\"nkeys\"}\n");
        return 1;
    }
    keys = p;
    p += (size_t)nkeys * 32u;
    if (p > end || rd(&p, end, &nacct, 2) != 0 || nacct > 256) {
        printf("{\"error\":\"nacct\"}\n");
        return 1;
    }
    acc = p;
    p += nacct;
    if (p > end || rd(&p, end, &dlen, 2) != 0) {
        printf("{\"error\":\"dlen\"}\n");
        return 1;
    }
    data = p;
    p += dlen;
    if (p > end || rd(&p, end, &blen, 2) != 0 || p + blen > end) {
        printf("{\"error\":\"blob\"}\n");
        return 1;
    }
    blob = p;
    if (swapix_from_flat(keys, nkeys, acc, nacct, data, dlen, &ix) != 0) {
        printf("{\"error\":\"swapix\"}\n");
        return 1;
    }
    if (ix.protocol == PROTO_DLMM) {
        dlmm_state_t before, after;
        dlmm_apply_result_t res;
        dlmm_swap_ix_t dix;
        int first = 1;
        if (load_dlmm_blob(blob, blen, &before) != 0) {
            printf("{\"error\":\"dlmm_blob\"}\n");
            return 1;
        }
        dix.amount_in = ix.amount_in;
        dix.min_amount_out = 0;
        dix.swap_for_y = ix.direction;
        if (dlmm_apply_swap(&before, &dix, &after, &res) != 0) {
            printf("{\"error\":\"dlmm_apply\"}\n");
            return 1;
        }
        printf("{\"kind\":\"dlmm\",\"active_id\":%d,\"active_after\":%d"
               ",\"reserve_x\":%llu,\"reserve_y\":%llu"
               ",\"vol_acc\":%u,\"vol_ref\":%u,\"idx_ref\":%d"
               ",\"last_upd\":%lld,\"fee\":%llu,\"protocol_fee\":%llu"
               ",\"amount_in\":%llu,\"direction\":%u,\"touched\":[",
               after.active_id, res.active_id_after,
               (unsigned long long)after.reserve_x,
               (unsigned long long)after.reserve_y,
               after.v_parameters.volatility_accumulator,
               after.v_parameters.volatility_reference,
               after.v_parameters.index_reference,
               (long long)after.v_parameters.last_update_timestamp,
               (unsigned long long)res.fee,
               (unsigned long long)res.protocol_fee,
               (unsigned long long)ix.amount_in,
               (unsigned)ix.direction);
        for (i = 0; i < after.nbin; i++) {
            uint16_t j;
            int ch = 1;
            for (j = 0; j < before.nbin; j++) {
                if (before.bins[j].id == after.bins[i].id
                    && before.bins[j].amount_x == after.bins[i].amount_x
                    && before.bins[j].amount_y == after.bins[i].amount_y) {
                    ch = 0;
                    break;
                }
            }
            if (!ch) {
                continue;
            }
            printf("%s{\"id\":%d,\"x\":%llu,\"y\":%llu}",
                   first ? "" : ",", after.bins[i].id,
                   (unsigned long long)after.bins[i].amount_x,
                   (unsigned long long)after.bins[i].amount_y);
            first = 0;
        }
        printf("]}\n");
        return 0;
    }
    if (ix.protocol == PROTO_PUMP) {
        pump_state_t before, after;
        pump_swap_result_t res;
        pump_swap_ix_t pix;
        if (load_pump_blob(blob, blen, &before) != 0) {
            printf("{\"error\":\"pump_blob\"}\n");
            return 1;
        }
        pix.amount_in = ix.amount_in;
        pix.min_amount_out = 0;
        pix.direction = ix.direction;
        if (pump_apply_swap(&before, &pix, &after, &res) != 0) {
            printf("{\"error\":\"pump_apply\"}\n");
            return 1;
        }
        printf("{\"kind\":\"pump\",\"reserve_base\":%llu,\"reserve_quote\":%llu"
               ",\"amount_in\":%llu,\"direction\":%u}\n",
               (unsigned long long)after.reserve_base,
               (unsigned long long)after.reserve_quote,
               (unsigned long long)ix.amount_in,
               (unsigned)ix.direction);
        return 0;
    }
    printf("{\"error\":\"proto\"}\n");
    return 1;
}
