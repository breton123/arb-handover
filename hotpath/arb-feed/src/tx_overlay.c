#include "tx_overlay.h"

#include "classify.h"
#include "frame.h"
#include "tx.h"

#include <string.h>

static const uint8_t DLMM_SWAP1[8] = {
    0xf8, 0xc6, 0x9e, 0x91, 0xe1, 0x75, 0x87, 0xc8
};
static const uint8_t PUMP_SELL[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};
static const uint8_t PUMP_BUY_EQ[8] = {
    0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70
};
static const uint8_t PUMP_BUY[8] = {
    0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
};
/* Anchor event discriminator — emit-only, not a pricing mutation. */
static const uint8_t ANCHOR_EVENT[8] = {
    0xe4, 0x45, 0xa5, 0x2e, 0x51, 0xcb, 0x9a, 0x1d
};

static int
header_ok(uint8_t nsig, uint8_t ro_s, uint8_t ro_u, uint16_t nkeys)
{
    (void)ro_s;
    (void)ro_u;
    return nsig >= 1 && nsig <= TX_SIG_MAX
        && nkeys >= nsig && nkeys <= TX_KEY_MAX;
}

static int
txo_keys(const uint8_t *p, uint32_t len, msg_keys_t *mk)
{
    uint32_t i;
    uint8_t nsig, b0, ro_s, ro_u, versioned = 0;
    uint16_t nkeys, used;

    if (p == NULL || mk == NULL || len < 65u) {
        return -1;
    }
    if (p[0] == TX_V1_PREFIX) {
        frame_v1_t L;
        if (frame_v1_layout(p, len, 0, &L) != 0) {
            return -1;
        }
        mk->keys = p + L.keys_off;
        mk->nkeys = L.naddr;
        mk->keys_off = (uint16_t)L.keys_off;
        mk->msg_off = (uint16_t)L.tx_end;
        mk->nsig = L.nsig;
        mk->versioned = FRAME_VER_V1;
        return 0;
    }
    if (len < 134u) {
        return -1;
    }
    if ((p[0] & 0x80u) != 0) {
        return -1;
    }
    nsig = p[0];
    if (nsig < 1 || nsig > TX_SIG_MAX) {
        return -1;
    }
    i = 1u + (uint32_t)nsig * TX_SIG_SZ;
    if (i >= len) {
        return -1;
    }
    b0 = p[i++];
    if ((b0 & 0x80u) != 0) {
        if ((b0 & 0x7fu) != 0 || i >= len || p[i] != nsig) {
            return -1;
        }
        versioned = 1;
        i++;
    } else if (b0 != nsig) {
        return -1;
    }
    if (i + 2u > len) {
        return -1;
    }
    ro_s = p[i++];
    ro_u = p[i++];
    if (cu16_dec(p + i, (uint16_t)(len - i), &nkeys, &used) != 0) {
        return -1;
    }
    i += used;
    if (!header_ok(nsig, ro_s, ro_u, nkeys)) {
        return -1;
    }
    if (i + 32u * (uint32_t)nkeys + TX_BLOCKHASH_SZ > len) {
        return -1;
    }
    mk->keys = p + i;
    mk->nkeys = nkeys;
    mk->keys_off = (uint16_t)i;
    mk->msg_off = (uint16_t)(1u + (uint32_t)nsig * TX_SIG_SZ);
    mk->nsig = nsig;
    mk->versioned = versioned;
    return 0;
}

static int
is_exact(uint8_t proto, const uint8_t *data, uint16_t dlen, uint8_t *variant)
{
    if (data == NULL || dlen < 24) {
        return 0;
    }
    if (proto == PROTO_DLMM) {
        if (memcmp(data, DLMM_SWAP2_DISC, 8) == 0) {
            *variant = 1;
            return 1;
        }
        if (memcmp(data, DLMM_SWAP1, 8) == 0) {
            *variant = 2;
            return 1;
        }
        return 0;
    }
    if (proto == PROTO_PUMP) {
        if (memcmp(data, PUMP_SELL, 8) == 0) {
            *variant = 3;
            return 1;
        }
        if (memcmp(data, PUMP_BUY_EQ, 8) == 0) {
            *variant = 4;
            return 1;
        }
        return 0;
    }
    return 0;
}

static int
is_event(const uint8_t *data, uint16_t dlen)
{
    return data != NULL && dlen >= 8 && memcmp(data, ANCHOR_EVENT, 8) == 0;
}

int
txo_scan(const uint8_t *p, uint32_t len, tx_overlay_t *out)
{
    msg_keys_t mk;
    uint32_t off;
    uint16_t ninstr, used, j;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->klass = TXO_FAIL;
    if (p == NULL || len < 65u) {
        return -1;
    }
    if (txo_keys(p, len, &mk) != 0) {
        uint16_t poff = 0;
        uint8_t proto = 0;
        if (find_prog_id(p, (uint16_t)len, &poff, &proto) != 0
            || msg_keys_from_prog(p, (uint16_t)len, poff, &mk) != 0) {
            return -1;
        }
    }
    if (mk.versioned == FRAME_VER_V1) {
        frame_v1_t L;
        uint8_t i;
        uint32_t start = 0;
        if (mk.keys_off >= TX_V1_HDR_SIZE) {
            start = (uint32_t)mk.keys_off - TX_V1_HDR_SIZE;
        }
        if (frame_v1_layout(p, len, start, &L) != 0) {
            return -1;
        }
        for (i = 0; i < L.ninstr; i++) {
            uint8_t prog, nacct, proto, variant = 0;
            uint16_t dlen;
            uint32_t acc_off, data_off;
            const uint8_t *pkey;
            txo_leg_t *leg;
            if (frame_v1_ix(p, &L, i, &prog, &nacct, &dlen, &acc_off,
                            &data_off) != 0) {
                return -1;
            }
            if (prog >= mk.nkeys) {
                out->n_unknown++;
                break;
            }
            pkey = mk.keys + (uint32_t)prog * 32u;
            proto = 0;
            if (id_eq32(pkey, PROG_DLMM)) {
                proto = PROTO_DLMM;
            } else if (id_eq32(pkey, PROG_PUMP)) {
                proto = PROTO_PUMP;
            }
            if (proto == 0) {
                continue;
            }
            if (out->n_leg >= TXO_LEG_MAX) {
                out->n_unknown++;
                continue;
            }
            leg = &out->legs[out->n_leg];
            memset(leg, 0, sizeof(*leg));
            leg->proto = proto;
            if (nacct >= 1) {
                uint8_t pool_i = p[acc_off];
                if (pool_i < mk.nkeys) {
                    memcpy(leg->pool, mk.keys + (uint32_t)pool_i * 32u, 32);
                }
            }
            if (dlen >= 8) {
                memcpy(leg->disc, p + data_off, 8);
            }
            if (is_event(p + data_off, dlen)) {
                continue;
            }
            if (is_exact(proto, p + data_off, dlen, &variant)) {
                uint64_t amt = 0, mino = 0;
                memcpy(&amt, p + data_off + 8, 8);
                memcpy(&mino, p + data_off + 16, 8);
                leg->exact = 1;
                leg->variant = variant;
                leg->amount_in = amt;
                leg->min_out = mino;
                if (variant == 3) {
                    leg->direction = PUMP_DIR_BASE_TO_QUOTE;
                    leg->have_dir = 1;
                } else if (variant == 4) {
                    leg->direction = PUMP_DIR_QUOTE_TO_BASE;
                    leg->have_dir = 1;
                }
                out->n_exact++;
                out->n_leg++;
            } else if (dlen >= 8 && memcmp(p + data_off, PUMP_BUY, 8) == 0) {
                leg->unknown_pricing = 1;
                out->n_unknown++;
                out->n_leg++;
            } else {
                leg->unknown_pricing = 1;
                out->n_unknown++;
                out->n_leg++;
            }
        }
        if (out->n_unknown != 0) {
            out->klass = TXO_UNKNOWN;
            out->ix_exact = out->n_exact > 0;
            out->tx_exact = 0;
        } else if (out->n_exact > 0) {
            out->klass = TXO_EXACT;
            out->ix_exact = 1;
            out->tx_exact = 1;
        } else {
            out->klass = TXO_FAIL;
        }
        return 0;
    }
    off = (uint32_t)mk.keys_off + 32u * (uint32_t)mk.nkeys + TX_BLOCKHASH_SZ;
    if (off >= len) {
        return -1;
    }
    if (cu16_dec(p + off, (uint16_t)(len - off), &ninstr, &used) != 0) {
        return -1;
    }
    if (ninstr == 0 || ninstr > TX_INSTR_MAX) {
        return -1;
    }
    off += used;
    for (j = 0; j < ninstr; j++) {
        uint8_t prog_idx, proto, variant = 0;
        uint16_t nacct, dlen;
        const uint8_t *pkey;
        txo_leg_t *leg;

        if (off + 1u > len) {
            return -1;
        }
        prog_idx = p[off++];
        if (prog_idx >= mk.nkeys) {
            /* V0 ALT-resident program: cannot classify. Fail closed. */
            out->n_unknown++;
            break;
        }
        if (cu16_dec(p + off, (uint16_t)(len - off), &nacct, &used) != 0) {
            return -1;
        }
        off += used;
        if (nacct > 255 || off + nacct > len) {
            return -1;
        }
        pkey = mk.keys + (uint32_t)prog_idx * 32u;
        proto = 0;
        if (id_eq32(pkey, PROG_DLMM)) {
            proto = PROTO_DLMM;
        } else if (id_eq32(pkey, PROG_PUMP)) {
            proto = PROTO_PUMP;
        }
        if (proto == 0) {
            off += nacct;
            if (cu16_dec(p + off, (uint16_t)(len - off), &dlen, &used) != 0) {
                return -1;
            }
            off += used;
            if (off + dlen > len) {
                return -1;
            }
            off += dlen;
            continue;
        }
        if (out->n_leg >= TXO_LEG_MAX) {
            out->n_unknown++;
            off += nacct;
            if (cu16_dec(p + off, (uint16_t)(len - off), &dlen, &used) != 0) {
                return -1;
            }
            off += used + dlen;
            continue;
        }
        leg = &out->legs[out->n_leg];
        memset(leg, 0, sizeof(*leg));
        leg->proto = proto;
        if (nacct >= 1) {
            uint8_t pool_i = p[off];
            if (pool_i < mk.nkeys) {
                memcpy(leg->pool, mk.keys + (uint32_t)pool_i * 32u, 32);
            }
        }
        off += nacct;
        if (cu16_dec(p + off, (uint16_t)(len - off), &dlen, &used) != 0) {
            return -1;
        }
        off += used;
        if (off + dlen > len) {
            return -1;
        }
        if (dlen >= 8) {
            memcpy(leg->disc, p + off, 8);
        }
        if (is_event(p + off, dlen)) {
            off += dlen;
            continue;
        }
        if (is_exact(proto, p + off, dlen, &variant)) {
            uint64_t amt = 0, mino = 0;
            memcpy(&amt, p + off + 8, 8);
            memcpy(&mino, p + off + 16, 8);
            leg->exact = 1;
            leg->variant = variant;
            leg->amount_in = amt;
            leg->min_out = mino;
            if (variant == 3) {
                leg->direction = PUMP_DIR_BASE_TO_QUOTE;
                leg->have_dir = 1;
            } else if (variant == 4) {
                leg->direction = PUMP_DIR_QUOTE_TO_BASE;
                leg->have_dir = 1;
            }
            out->n_exact++;
            out->n_leg++;
        } else if (dlen >= 8 && memcmp(p + off, PUMP_BUY, 8) == 0) {
            leg->unknown_pricing = 1;
            out->n_unknown++;
            out->n_leg++;
        } else {
            /* Unsupported DLMM/Pump instruction — liquidity / special. */
            leg->unknown_pricing = 1;
            out->n_unknown++;
            out->n_leg++;
        }
        off += dlen;
    }
    if (out->n_unknown != 0) {
        out->klass = TXO_UNKNOWN;
        out->ix_exact = out->n_exact > 0;
        out->tx_exact = 0;
    } else if (out->n_exact > 0) {
        out->klass = TXO_EXACT;
        out->ix_exact = 1;
        out->tx_exact = 1;
    } else {
        out->klass = TXO_FAIL;
    }
    return 0;
}

void
txo_bind_watched(tx_overlay_t *ov, const uint8_t *pool)
{
    uint8_t i, unk_here = 0, exact_here = 0, zero_pool_unk = 0;

    if (ov == NULL || pool == NULL) {
        return;
    }
    for (i = 0; i < ov->n_leg; i++) {
        int here = (memcmp(ov->legs[i].pool, pool, 32) == 0);
        int empty = 1;
        uint8_t k;
        for (k = 0; k < 32; k++) {
            if (ov->legs[i].pool[k] != 0) {
                empty = 0;
                break;
            }
        }
        if (ov->legs[i].unknown_pricing && empty) {
            zero_pool_unk = 1;
        }
        if (!here) {
            continue;
        }
        if (ov->legs[i].unknown_pricing) {
            unk_here++;
        }
        if (ov->legs[i].exact) {
            exact_here++;
        }
    }
    ov->ix_exact = exact_here > 0 ? 1 : 0;
    if (unk_here || zero_pool_unk) {
        ov->klass = TXO_UNKNOWN;
        ov->tx_exact = 0;
    } else if (exact_here > 0) {
        ov->klass = TXO_EXACT;
        ov->tx_exact = 1;
    } else {
        ov->klass = TXO_FAIL;
        ov->tx_exact = 0;
    }
}

void
txo_stamp_n(tx_overlay_t *ov, const swapix_t *nix)
{
    uint8_t i, stamped = 0, need = 0;

    if (ov == NULL || nix == NULL) {
        return;
    }
    for (i = 0; i < ov->n_leg; i++) {
        if (!ov->legs[i].exact) {
            continue;
        }
        if (memcmp(ov->legs[i].pool, nix->pool, 32) != 0) {
            continue;
        }
        need++;
        if (ov->legs[i].have_dir) {
            stamped++;
            continue;
        }
        if (ov->n_exact == 1 || ov->legs[i].amount_in == nix->amount_in) {
            ov->legs[i].direction = nix->direction;
            ov->legs[i].have_dir = 1;
            stamped++;
        }
    }
    if (need > stamped) {
        ov->tx_exact = 0;
        if (ov->ix_exact) {
            ov->klass = TXO_IX_ONLY;
        }
    }
}

int
txo_apply(tx_overlay_t *ov, const uint8_t *pool, uint8_t proto,
          const dlmm_state_t *dlmm_before, const pump_state_t *pump_before)
{
    uint8_t i;

    if (ov == NULL || pool == NULL) {
        return -1;
    }
    if (ov->klass != TXO_EXACT || !ov->tx_exact) {
        return -1;
    }
    if (proto == PROTO_DLMM && dlmm_before != NULL) {
        dlmm_state_t cur = *dlmm_before;
        ov->have_dlmm = 1;
        ov->s_dlmm = cur;
        for (i = 0; i < ov->n_leg; i++) {
            dlmm_swap_ix_t ix;
            dlmm_apply_result_t res;
            if (!ov->legs[i].exact || ov->legs[i].proto != PROTO_DLMM) {
                continue;
            }
            if (memcmp(ov->legs[i].pool, pool, 32) != 0) {
                continue;
            }
            if (!ov->legs[i].have_dir) {
                ov->klass = TXO_IX_ONLY;
                ov->tx_exact = 0;
                return -1;
            }
            memset(&ix, 0, sizeof(ix));
            ix.amount_in = ov->legs[i].amount_in;
            ix.min_amount_out = 0;
            ix.swap_for_y = ov->legs[i].direction;
            if (dlmm_apply_swap(&cur, &ix, &ov->s_dlmm, &res) != 0) {
                ov->klass = TXO_FAIL;
                ov->tx_exact = 0;
                return -1;
            }
            ov->last_fee = res.fee;
            ov->last_pfee = res.protocol_fee;
            ov->last_active_after = res.active_id_after;
            cur = ov->s_dlmm;
        }
        return 0;
    }
    if (proto == PROTO_PUMP && pump_before != NULL) {
        pump_state_t cur = *pump_before;
        ov->have_pump = 1;
        ov->s_pump = cur;
        for (i = 0; i < ov->n_leg; i++) {
            pump_swap_ix_t ix;
            pump_swap_result_t res;
            if (!ov->legs[i].exact || ov->legs[i].proto != PROTO_PUMP) {
                continue;
            }
            if (memcmp(ov->legs[i].pool, pool, 32) != 0) {
                continue;
            }
            memset(&ix, 0, sizeof(ix));
            ix.amount_in = ov->legs[i].amount_in;
            ix.direction = ov->legs[i].direction;
            if (pump_apply_swap(&cur, &ix, &ov->s_pump, &res) != 0) {
                ov->klass = TXO_FAIL;
                ov->tx_exact = 0;
                return -1;
            }
            ov->last_fee = res.fee;
            cur = ov->s_pump;
        }
        return 0;
    }
    return -1;
}
