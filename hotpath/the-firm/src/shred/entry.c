#include "shred/entry.h"

#include <string.h>

static uint64_t
u64le(const uint8_t *p)
{
    uint64_t v;

    memcpy(&v, p, 8);
    return v;
}

int
shred_entries_parse(const uint8_t *buf, uint32_t len, shred_entry_cb cb,
                    void *user, uint32_t *consumed, int *rc_out)
{
    uint32_t off = 0, e, n_tx_all = 0;
    uint64_t nent;

    if (consumed != NULL) {
        *consumed = 0;
    }
    if (rc_out != NULL) {
        *rc_out = ENT_OK;
    }
    if (buf == NULL) {
        if (rc_out != NULL) {
            *rc_out = ENT_BAD_HEADER;
        }
        return ENT_BAD_HEADER;
    }
    if (len < 8u) {
        if (rc_out != NULL) {
            *rc_out = ENT_NEED_MORE;
        }
        return ENT_NEED_MORE;
    }
    nent = u64le(buf);
    if (nent > ENT_VEC_MAX) {
        if (rc_out != NULL) {
            *rc_out = ENT_BAD_HEADER;
        }
        return ENT_BAD_HEADER;
    }
    off = 8u;
    for (e = 0; e < (uint32_t)nent; e++) {
        uint64_t ntx;
        uint32_t t, ent_off = off;

        if (off + 48u > len) {
            if (consumed != NULL) {
                *consumed = off;
            }
            if (rc_out != NULL) {
                *rc_out = ENT_NEED_MORE;
            }
            return ENT_NEED_MORE;
        }
        ntx = u64le(buf + off + 40u);
        if (ntx > ENT_TX_MAX) {
            if (consumed != NULL) {
                *consumed = off;
            }
            if (rc_out != NULL) {
                *rc_out = ENT_BAD_HEADER;
            }
            return ENT_BAD_HEADER;
        }
        off += 48u;
        for (t = 0; t < (uint32_t)ntx; t++) {
            txframe_t tf;
            int rc = txframe_parse(buf + off, len - off, &tf);

            if (rc == TXF_NEED_MORE_BYTES) {
                if (consumed != NULL) {
                    *consumed = off;
                }
                if (rc_out != NULL) {
                    *rc_out = ENT_NEED_MORE;
                }
                return ENT_NEED_MORE;
            }
            if (rc != TXF_OK) {
                if (consumed != NULL) {
                    *consumed = off;
                }
                if (rc_out != NULL) {
                    *rc_out = ENT_STREAM_DESYNC;
                }
                return ENT_STREAM_DESYNC;
            }
            tf.stream_off = off;
            if (cb != NULL) {
                shred_entry_hit_t hit;

                memset(&hit, 0, sizeof(hit));
                hit.entry_ord = e;
                hit.tx_ord = t;
                hit.entry_off = ent_off;
                hit.entry_len = 0;
                hit.entry_tx_count = (uint32_t)ntx;
                hit.consumed = off + tf.tx_len;
                hit.n_tx = ++n_tx_all;
                hit.bytes = buf + off;
                hit.tx = tf;
                cb(user, &hit);
            } else {
                n_tx_all++;
            }
            off += tf.tx_len;
        }
    }
    if (consumed != NULL) {
        *consumed = off;
    }
    if (rc_out != NULL) {
        *rc_out = ENT_OK;
    }
    return ENT_OK;
}
