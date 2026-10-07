#include "tx_n.h"
#include "classify.h"
#include "proto.h"

#include <string.h>

static const uint8_t *const PROG_N[CLASSIFY_N_MAX] = {
    PROG_DLMM, PROG_PUMP, PROG_CLMM, PROG_CPMM, PROG_DAMM, PROG_ORCA
};

static const uint8_t PROTO_N_ID[CLASSIFY_N_MAX] = {
    PROTO_DLMM, PROTO_PUMP, PROTO_CLMM, PROTO_CPMM, PROTO_DAMM, PROTO_ORCA
};

int
key_is_dex_n(const uint8_t k[32], uint32_t n_ids)
{
    uint32_t i;

    if (k == NULL || n_ids == 0) {
        return 0;
    }
    if (n_ids > CLASSIFY_N_MAX) {
        n_ids = CLASSIFY_N_MAX;
    }
    for (i = 0; i < n_ids; i++) {
        if (id_eq32(k, PROG_N[i])) {
            return (int)PROTO_N_ID[i];
        }
    }
    return 0;
}

int
find_prog_id_n(const uint8_t *payload, size_t len, uint32_t n_ids,
               size_t *off, uint8_t *proto)
{
    size_t i;
    size_t last;

    if (payload == NULL || len < 32 || off == NULL || proto == NULL || n_ids == 0) {
        return -1;
    }
    if (n_ids > CLASSIFY_N_MAX) {
        n_ids = CLASSIFY_N_MAX;
    }
    last = len - 32;
    for (i = 0; i <= last; i++) {
        uint32_t n;
        for (n = 0; n < n_ids; n++) {
            if (id_eq32(payload + i, PROG_N[n])) {
                *off = i;
                *proto = PROTO_N_ID[n];
                return 0;
            }
        }
    }
    return -1;
}

int
msg_first_dex_pool_n(const uint8_t *payload, size_t len, uint32_t n_ids,
                     uint8_t *proto, const uint8_t **pool)
{
    msg_keys_t mk;
    size_t poff;
    uint8_t pfound;
    pool_cand_t cand;
    uint32_t off;
    uint16_t ninstr;
    uint16_t used;
    uint16_t j;

    if (payload == NULL || proto == NULL || pool == NULL) {
        return -1;
    }
    if (find_prog_id_n(payload, len, n_ids, &poff, &pfound) != 0) {
        return -1;
    }
    if (poff > 0xffffu || len > 0xffffu) {
        return -1;
    }
    if (msg_keys_from_prog(payload, (uint16_t)len, (uint16_t)poff, &mk) != 0) {
        return -1;
    }
    off = (uint32_t)mk.keys_off + 32u * (uint32_t)mk.nkeys;
    if (off + TX_BLOCKHASH_SZ > len) {
        return -1;
    }
    off += TX_BLOCKHASH_SZ;
    if (cu16_dec(payload + off, (uint16_t)(len - off), &ninstr, &used) != 0) {
        return -1;
    }
    if (ninstr == 0 || ninstr > TX_INSTR_MAX) {
        return -1;
    }
    off += used;
    for (j = 0; j < ninstr; j++) {
        uint8_t prog_idx;
        uint16_t nacct;
        uint16_t dlen;
        int pr;

        if (off + 1u > len) {
            return -1;
        }
        prog_idx = payload[off];
        off += 1;
        if (prog_idx == 0 || prog_idx >= mk.nkeys) {
            return -1;
        }
        if (cu16_dec(payload + off, (uint16_t)(len - off), &nacct, &used) != 0) {
            return -1;
        }
        off += used;
        if (nacct > 255 || off + nacct > len) {
            return -1;
        }
        pr = key_is_dex_n(mk.keys + (uint32_t)prog_idx * 32u, n_ids);
        if (pr != 0 && nacct >= 1) {
            uint8_t pool_idx = payload[off];
            if (pool_idx < mk.nkeys) {
                cand.protocol = (uint8_t)pr;
                cand.key = mk.keys + (uint32_t)pool_idx * 32u;
                *proto = cand.protocol;
                *pool = cand.key;
                return 0;
            }
        }
        off += nacct;
        if (cu16_dec(payload + off, (uint16_t)(len - off), &dlen, &used) != 0) {
            return -1;
        }
        off += used;
        if (off + dlen > len) {
            return -1;
        }
        off += dlen;
    }
    return -1;
}
