#include "shred/complete.h"

#include "ingress/decode.h"

#include <string.h>

static int
try_at(const uint8_t *p, uint32_t len, uint32_t start, uint32_t *end)
{
    ordered_tx_t tx;
    int rc;

    if (start >= len) {
        return SHRED_TX_WAIT;
    }
    rc = ingress_decode_tx(p + start, len - start, 0, &tx);
    if (rc == DEC_INCOMPLETE) {
        return SHRED_TX_WAIT;
    }
    if (rc == DEC_INVALID) {
        return SHRED_TX_BAD;
    }
    /* DEC_OK or DEC_ALT: bytes are framed */
    /* framed enough to name a tx; ALT still a complete blob */
    *end = start + tx.raw_len;
    if (tx.raw_len == 0 || *end > len) {
        *end = len;
    }
    return SHRED_TX_OK;
}

int
shred_first_tx(const uint8_t *pkt, uint16_t len, uint64_t *slot,
               const uint8_t **tx, uint32_t *tx_len)
{
    shred_view_t v;
    uint16_t size, body;
    const uint8_t *p;
    uint32_t end = 0;
    int rc;

    if (pkt == NULL || tx == NULL || tx_len == NULL) {
        return SHRED_TX_BAD;
    }
    if (shred_parse(pkt, len, &v) != 0 || !shred_is_data(v.type)) {
        return SHRED_TX_BAD;
    }
    if (len < SHRED_DATA_HDR_SZ || (pkt[SHRED_OFF_FLAGS] & SHRED_DATA_COMPLETE) == 0) {
        return SHRED_TX_WAIT;
    }
    size = shred_load_u16_le(pkt + SHRED_OFF_SIZE);
    body = (uint16_t)(len - SHRED_DATA_HDR_SZ);
    if (size != 0 && size < body) {
        body = size;
    }
    p = pkt + SHRED_DATA_HDR_SZ;
    if (slot != NULL) {
        *slot = v.slot;
    }

    /* raw tx */
    rc = try_at(p, body, 0, &end);
    if (rc == SHRED_TX_OK) {
        *tx = p;
        *tx_len = end;
        return SHRED_TX_OK;
    }

    /* bincode Entry: u64 hashes + 32 hash + u64 ntx */
    if (body >= 48u) {
        uint64_t ntx;
        memcpy(&ntx, p + 40, 8);
        if (ntx >= 1u && ntx < 64u) {
            rc = try_at(p, body, 48, &end);
            if (rc == SHRED_TX_OK) {
                *tx = p + 48;
                *tx_len = end - 48;
                return SHRED_TX_OK;
            }
        }
    }
    return rc == SHRED_TX_WAIT ? SHRED_TX_WAIT : SHRED_TX_BAD;
}
