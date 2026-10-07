#include "transitions/token.h"

#include <string.h>

static int
same32(const uint8_t a[32], const uint8_t b[32])
{
    return memcmp(a, b, 32) == 0;
}

static int
credit(state_token_t *t, uint64_t n)
{
    if (t == NULL || !t->live) {
        return -1;
    }
    if (UINT64_MAX - t->amount < n) {
        return -1;
    }
    t->amount += n;
    return 0;
}

static int
debit(state_token_t *t, uint64_t n)
{
    if (t == NULL || !t->live || t->amount < n) {
        return -1;
    }
    t->amount -= n;
    return 0;
}

static int
touch_token(tx_overlay_t *ov, const compact_state_t *st, uint32_t id,
            uint8_t allow_dead, overlay_token_row_t **out)
{
    if (id == STATE_ACCT_NONE) {
        *out = NULL;
        return 0;
    }
    return overlay_ensure_token(ov, st, id, allow_dead, out);
}

int
overlay_apply_pump_tokens(tx_overlay_t *ov, const compact_state_t *st,
                          const ordered_ix_t *ix,
                          const pump_swap_result_t *res,
                          const pump_state_t *after)
{
    overlay_token_row_t *src = NULL, *dst = NULL;
    overlay_token_row_t *vb = NULL, *vq = NULL, *fp = NULL, *fc = NULL;

    if (ov == NULL || ix == NULL || res == NULL || after == NULL) {
        return -1;
    }
    if (touch_token(ov, st, ix->src_token, 0, &src) != 0
        || touch_token(ov, st, ix->dst_token, 0, &dst) != 0
        || touch_token(ov, st, ix->vault_base, 0, &vb) != 0
        || touch_token(ov, st, ix->vault_quote, 0, &vq) != 0
        || touch_token(ov, st, ix->fee_proto, 0, &fp) != 0
        || touch_token(ov, st, ix->fee_creator, 0, &fc) != 0) {
        ov->status = OV_FAIL;
        return -1;
    }
    if (src != NULL) {
        if (debit(&src->tok, res->amount_in) != 0) {
            ov->status = OV_FAIL;
            return -1;
        }
        src->dirty = 1;
    }
    if (dst != NULL) {
        if (credit(&dst->tok, res->amount_out) != 0) {
            ov->status = OV_FAIL;
            return -1;
        }
        dst->dirty = 1;
    }
    if (vb != NULL) {
        vb->tok.amount = after->reserve_base;
        vb->dirty = 1;
    }
    if (vq != NULL) {
        vq->tok.amount = after->reserve_quote;
        vq->dirty = 1;
    }
    if (fp != NULL) {
        if (credit(&fp->tok, res->protocol_fee) != 0) {
            ov->status = OV_FAIL;
            return -1;
        }
        fp->dirty = 1;
    }
    if (fc != NULL) {
        if (credit(&fc->tok, res->creator_fee) != 0) {
            ov->status = OV_FAIL;
            return -1;
        }
        fc->dirty = 1;
    }
    return 0;
}

static int
apply_xfer(tx_overlay_t *ov, const compact_state_t *st, const ordered_ix_t *ix)
{
    overlay_token_row_t *src, *dst;

    if (overlay_ensure_token(ov, st, ix->src_token, 0, &src) != 0
        || overlay_ensure_token(ov, st, ix->dst_token, 0, &dst) != 0) {
        return -1;
    }
    if (!same32(src->tok.mint, dst->tok.mint)
        || src->tok.token_program != dst->tok.token_program) {
        return -1;
    }
    if (debit(&src->tok, ix->amount_in) != 0
        || credit(&dst->tok, ix->amount_in) != 0) {
        return -1;
    }
    src->dirty = 1;
    dst->dirty = 1;
    return 0;
}

static int
apply_sync(tx_overlay_t *ov, const compact_state_t *st, const ordered_ix_t *ix)
{
    overlay_token_row_t *t;

    if (overlay_ensure_token(ov, st, ix->src_token, 0, &t) != 0) {
        return -1;
    }
    if ((t->tok.flags & TOKEN_FLAG_WSOL) == 0) {
        return -1;
    }
    t->tok.amount = t->tok.native_reserve;
    t->dirty = 1;
    return 0;
}

static int
apply_close(tx_overlay_t *ov, const compact_state_t *st, const ordered_ix_t *ix)
{
    overlay_token_row_t *src;
    uint64_t amt;

    if (overlay_ensure_token(ov, st, ix->src_token, 0, &src) != 0) {
        return -1;
    }
    amt = src->tok.amount;
    if (ix->dst_sys != STATE_ACCT_NONE) {
        overlay_sys_row_t *d;

        if (overlay_ensure_sys(ov, st, ix->dst_sys, &d) != 0) {
            return -1;
        }
        if (UINT64_MAX - d->sys.lamports < amt) {
            return -1;
        }
        d->sys.lamports += amt;
        d->dirty = 1;
    } else if (ix->dst_token != STATE_ACCT_NONE) {
        overlay_token_row_t *d;

        if (overlay_ensure_token(ov, st, ix->dst_token, 0, &d) != 0) {
            return -1;
        }
        if (UINT64_MAX - d->tok.native_reserve < amt) {
            return -1;
        }
        d->tok.native_reserve += amt;
        d->dirty = 1;
    } else {
        return -1;
    }
    src->tok.amount = 0;
    src->tok.native_reserve = 0;
    src->tok.live = 0;
    src->dirty = 1;
    return 0;
}

static int
apply_ata(tx_overlay_t *ov, const compact_state_t *st, const ordered_ix_t *ix)
{
    overlay_token_row_t *t;

    if (overlay_ensure_token(ov, st, ix->dst_token, 1, &t) != 0) {
        return -1;
    }
    if (t->tok.live) {
        if (!same32(t->tok.mint, ix->mint) || !same32(t->tok.owner, ix->owner)) {
            return -1;
        }
        return 0;
    }
    memcpy(t->tok.mint, ix->mint, 32);
    memcpy(t->tok.owner, ix->owner, 32);
    t->tok.amount = 0;
    t->tok.native_reserve = 0;
    t->tok.token_program = TOKEN_PROG_SPL;
    t->tok.extensions_mask = 0;
    t->tok.live = 1;
    t->dirty = 1;
    return 0;
}

static int
apply_sys_xfer(tx_overlay_t *ov, const compact_state_t *st,
               const ordered_ix_t *ix)
{
    overlay_sys_row_t *src;

    if (overlay_ensure_sys(ov, st, ix->src_sys, &src) != 0) {
        return -1;
    }
    if (src->sys.lamports < ix->amount_in) {
        return -1;
    }
    src->sys.lamports -= ix->amount_in;
    src->dirty = 1;
    if (ix->dst_sys != STATE_ACCT_NONE) {
        overlay_sys_row_t *dst;

        if (overlay_ensure_sys(ov, st, ix->dst_sys, &dst) != 0) {
            return -1;
        }
        if (UINT64_MAX - dst->sys.lamports < ix->amount_in) {
            return -1;
        }
        dst->sys.lamports += ix->amount_in;
        dst->dirty = 1;
    }
    if (ix->dst_token != STATE_ACCT_NONE) {
        overlay_token_row_t *w;

        if (overlay_ensure_token(ov, st, ix->dst_token, 0, &w) != 0) {
            return -1;
        }
        if ((w->tok.flags & TOKEN_FLAG_WSOL) == 0) {
            return -1;
        }
        if (UINT64_MAX - w->tok.native_reserve < ix->amount_in) {
            return -1;
        }
        w->tok.native_reserve += ix->amount_in;
        w->dirty = 1;
    }
    if (ix->dst_sys == STATE_ACCT_NONE && ix->dst_token == STATE_ACCT_NONE) {
        return -1;
    }
    return 0;
}

int
overlay_apply_token_ix(tx_overlay_t *ov, const compact_state_t *st,
                       const ordered_ix_t *ix)
{
    int rc;

    if (ov == NULL || ix == NULL) {
        return -1;
    }
    switch (ix->kind) {
    case IX_KIND_TOKEN_XFER:
        if (ix->src_token == STATE_ACCT_NONE
            || ix->dst_token == STATE_ACCT_NONE) {
            /* User ATAs are not in CompactState; vault amounts already
             * follow the Pump overlay. Do not invent balances. */
            return 0;
        }
        rc = apply_xfer(ov, st, ix);
        break;
    case IX_KIND_TOKEN_SYNC:
        if (ix->src_token == STATE_ACCT_NONE) {
            return 0;
        }
        rc = apply_sync(ov, st, ix);
        break;
    case IX_KIND_TOKEN_CLOSE:
        if (ix->src_token == STATE_ACCT_NONE) {
            return 0;
        }
        rc = apply_close(ov, st, ix);
        break;
    case IX_KIND_ATA_CREATE:
    case IX_KIND_ATA_INIT:
        if (ix->dst_token == STATE_ACCT_NONE) {
            return 0;
        }
        rc = apply_ata(ov, st, ix);
        break;
    case IX_KIND_SYS_TRANSFER:
        if (ix->src_sys == STATE_ACCT_NONE && ix->dst_sys == STATE_ACCT_NONE
            && ix->dst_token == STATE_ACCT_NONE) {
            return 0;
        }
        rc = apply_sys_xfer(ov, st, ix);
        break;
    default:
        ov->status = OV_UNKNOWN;
        return -1;
    }
    if (rc != 0) {
        ov->status = OV_FAIL;
    }
    return rc;
}
