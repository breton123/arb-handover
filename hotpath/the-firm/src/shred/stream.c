#include "shred/stream.h"

#include "ingress/decode.h"
#include "shred/sig.h"

#include <stdlib.h>
#include <string.h>

static uint64_t
u64le(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static int
is_pump(const ordered_tx_t *tx)
{
    uint32_t i;

    for (i = 0; i < tx->n_ix; i++) {
        if (tx->ix[i].proto == PROTO_PUMP
            && (tx->ix[i].kind == IX_KIND_PUMP_SELL
                || tx->ix[i].kind == IX_KIND_PUMP_BUY_EQ
                || tx->ix[i].kind == IX_KIND_PUMP_BUY)) {
            return 1;
        }
    }
    return 0;
}

int
shred_slot_stream_init(shred_slot_stream_t *s)
{
    if (s == NULL) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->buf = calloc(1, SHRED_STREAM_BUF);
    s->emit = calloc(1, SHRED_STREAM_BUF);
    if (s->buf == NULL || s->emit == NULL) {
        shred_slot_stream_free(s);
        return -1;
    }
    s->live = 1;
    return 0;
}

void
shred_slot_stream_reset(shred_slot_stream_t *s)
{
    uint8_t *buf, *emit;

    if (s == NULL) {
        return;
    }
    buf = s->buf;
    emit = s->emit;
    memset(s, 0, sizeof(*s));
    s->buf = buf;
    s->emit = emit;
    s->live = 1;
    if (buf != NULL) {
        memset(buf, 0, SHRED_STREAM_BUF);
    }
    if (emit != NULL) {
        memset(emit, 0, SHRED_STREAM_BUF);
    }
}

void
shred_slot_stream_free(shred_slot_stream_t *s)
{
    if (s == NULL) {
        return;
    }
    free(s->buf);
    free(s->emit);
    memset(s, 0, sizeof(*s));
}

uint32_t
shred_slot_stream_leftover(const shred_slot_stream_t *s)
{
    if (s == NULL || s->buf_len < s->parse_off) {
        return 0;
    }
    return s->buf_len - s->parse_off;
}

static void
compact(shred_slot_stream_t *s)
{
    uint32_t rem;

    if (s->parse_off == 0) {
        return;
    }
    rem = s->buf_len - s->parse_off;
    if (rem != 0) {
        memmove(s->buf, s->buf + s->parse_off, rem);
    }
    s->buf_len = rem;
    s->parse_off = 0;
}

int
shred_slot_stream_append(shred_slot_stream_t *s, const uint8_t *payload,
                         uint16_t plen, uint64_t tsc, int recovered,
                         shred_stream_stats_t *st)
{
    if (s == NULL || s->frozen || payload == NULL) {
        return -1;
    }
    if ((uint32_t)plen + s->buf_len > SHRED_STREAM_BUF) {
        compact(s);
    }
    if ((uint32_t)plen + s->buf_len > SHRED_STREAM_BUF) {
        s->frozen = 1;
        if (st != NULL) {
            st->frozen++;
        }
        return -1;
    }
    memcpy(s->buf + s->buf_len, payload, plen);
    s->buf_len += plen;
    s->wm++;
    s->n_shreds++;
    if (recovered) {
        s->n_fec++;
    }
    if (s->tsc_origin == 0 && tsc != 0) {
        s->tsc_origin = tsc;
    }
    if (s->tsc_first == 0 && tsc != 0) {
        s->tsc_first = tsc;
    }
    s->tsc_last = tsc;
    s->tsc_contig = tsc;
    if (st != NULL) {
        st->slot_stream_bytes += plen;
        st->contig_bytes += plen;
        st->watermark_advances++;
    }
    return 0;
}

static int
emit_tx(shred_slot_stream_t *s, const uint8_t *p, uint32_t len,
        const ordered_tx_t *tx, shred_stream_stats_t *st)
{
    int vr;

    if (s->emit_n >= SHRED_STREAM_TX_MAX
        || s->emit_len + len > SHRED_STREAM_BUF) {
        return -1;
    }
    s->emit_off[s->emit_n] = s->emit_len;
    s->emit_len_i[s->emit_n] = len;
    s->emit_ent[s->emit_n] = (s->entry_ord > 0) ? (s->entry_ord - 1u) : 0;
    s->emit_txi[s->emit_n] = s->tx_in_ent;
    s->tx_in_ent++;
    memcpy(s->emit + s->emit_len, p, len);
    s->emit_len += len;
    s->emit_n++;
    if (st == NULL) {
        return 0;
    }
    st->txs++;
    if (tx->encoding == TX_ENC_V0) {
        st->tx_v0++;
    } else if (tx->encoding == TX_ENC_V1) {
        st->tx_v1++;
    } else {
        st->tx_legacy++;
    }
    if (is_pump(tx)) {
        st->pump++;
    }
    vr = shred_tx_sig_verify(p, len);
    if (vr == 1) {
        st->sig_verify_ok++;
    } else if (vr == 0) {
        st->sig_verify_fail++;
    }
    return 0;
}

void
shred_slot_stream_parse(shred_slot_stream_t *s, uint64_t slot,
                        shred_stream_stats_t *st)
{
    if (s == NULL || s->frozen || s->buf == NULL) {
        return;
    }
    s->slot = slot;
    for (;;) {
        uint32_t rem = s->buf_len - s->parse_off;
        const uint8_t *p = s->buf + s->parse_off;

        if (s->pst == STREAM_ST_VEC) {
            uint64_t nent;

            if (rem < 8u) {
                break;
            }
            nent = u64le(p);
            if (nent > SHRED_STREAM_VEC_MAX) {
                s->frozen = 1;
                if (st != NULL) {
                    st->frozen++;
                }
                break;
            }
            s->parse_off += 8u;
            s->vec_left = nent;
            s->pst = (nent == 0) ? STREAM_ST_VEC : STREAM_ST_ENT;
            continue;
        }
        if (s->pst == STREAM_ST_ENT) {
            uint64_t ntx;

            if (rem < 48u) {
                break;
            }
            ntx = u64le(p + 40);
            if (ntx > SHRED_STREAM_VEC_MAX) {
                s->frozen = 1;
                if (st != NULL) {
                    st->frozen++;
                }
                break;
            }
            s->parse_off += 48u;
            s->entry_ord++;
            s->tx_in_ent = 0;
            if (st != NULL) {
                st->entries++;
                st->entries_incremental++;
            }
            s->tsc_entry = s->tsc_last;
            s->tx_left = ntx;
            if (ntx == 0) {
                if (s->vec_left > 0) {
                    s->vec_left--;
                }
                s->pst = (s->vec_left == 0) ? STREAM_ST_VEC : STREAM_ST_ENT;
            } else {
                s->pst = STREAM_ST_TX;
            }
            continue;
        }
        {
            ordered_tx_t tx;
            int rc;

            rc = ingress_decode_tx(p, rem, slot, &tx);
            if (rc == DEC_INCOMPLETE) {
                break;
            }
            if ((rc != DEC_OK && rc != DEC_ALT) || tx.raw_len == 0
                || tx.raw_len > rem) {
                s->frozen = 1;
                if (st != NULL) {
                    st->frozen++;
                }
                break;
            }
            if (emit_tx(s, p, tx.raw_len, &tx, st) != 0) {
                break;
            }
            s->parse_off += tx.raw_len;
            if (s->tx_left > 0) {
                s->tx_left--;
            }
            if (s->tx_left == 0) {
                if (s->vec_left > 0) {
                    s->vec_left--;
                }
                s->pst = (s->vec_left == 0) ? STREAM_ST_VEC : STREAM_ST_ENT;
            }
        }
    }
    if (s->parse_off > (SHRED_STREAM_BUF / 2u)) {
        compact(s);
    }
}

int
shred_slot_stream_take(shred_slot_stream_t *s, uint8_t *dst, uint32_t dst_cap,
                       uint32_t *offs, uint32_t *lens, uint32_t *ents,
                       uint32_t *txis, uint32_t max_tx, uint32_t *n_tx,
                       uint32_t *blen, uint32_t *n_shreds, uint32_t *n_fec)
{
    uint32_t n, i;

    if (s == NULL || dst == NULL || n_tx == NULL || blen == NULL) {
        return -1;
    }
    n = s->emit_n;
    if (n > max_tx) {
        n = max_tx;
    }
    if (s->emit_len > dst_cap) {
        return -1;
    }
    memcpy(dst, s->emit, s->emit_len);
    for (i = 0; i < n; i++) {
        offs[i] = s->emit_off[i];
        lens[i] = s->emit_len_i[i];
        if (ents != NULL) {
            ents[i] = s->emit_ent[i];
        }
        if (txis != NULL) {
            txis[i] = s->emit_txi[i];
        }
    }
    *n_tx = n;
    *blen = s->emit_len;
    if (n_shreds != NULL) {
        *n_shreds = s->n_shreds;
    }
    if (n_fec != NULL) {
        *n_fec = s->n_fec;
    }
    s->emit_n = 0;
    s->emit_len = 0;
    s->n_shreds = 0;
    s->n_fec = 0;
    s->tsc_first = 0;
    return (n != 0) ? 1 : 0;
}

int
shred_stream_parse(const uint8_t *buf, uint32_t len, uint64_t slot,
                   shred_stream_hit_t *out)
{
    shred_slot_stream_t s;
    shred_stream_stats_t st;
    uint32_t i;

    if (buf == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    memset(&st, 0, sizeof(st));
    memset(&s, 0, sizeof(s));
    if (shred_slot_stream_init(&s) != 0) {
        return -1;
    }
    if (len > SHRED_STREAM_BUF) {
        shred_slot_stream_free(&s);
        return -1;
    }
    memcpy(s.buf, buf, len);
    s.buf_len = len;
    shred_slot_stream_parse(&s, slot, &st);
    out->n_ent = (uint32_t)st.entries;
    out->n_tx = s.emit_n;
    out->n_legacy = (uint32_t)st.tx_legacy;
    out->n_v0 = (uint32_t)st.tx_v0;
    out->n_v1 = (uint32_t)st.tx_v1;
    out->n_pump = (uint32_t)st.pump;
    out->consumed = s.parse_off;
    for (i = 0; i < s.emit_n && i < SHRED_STREAM_TX_MAX; i++) {
        out->tx_off[i] = s.emit_off[i];
        out->tx_len[i] = s.emit_len_i[i];
    }
    shred_slot_stream_free(&s);
    return (out->n_ent != 0 || out->n_tx != 0) ? 0 : -1;
}
