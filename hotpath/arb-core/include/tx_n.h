#ifndef ARB_CORE_TX_N_H
#define ARB_CORE_TX_N_H

#include <stddef.h>
#include <stdint.h>

#include "tx.h"

/*
 * CORE-009 framing beside locked find_prog_id / key_is_dex.
 * n_ids = 2..6 in the same expansion order as classify_n.
 */

int find_prog_id_n(const uint8_t *payload, size_t len, uint32_t n_ids,
                   size_t *off, uint8_t *proto);

int key_is_dex_n(const uint8_t k[32], uint32_t n_ids);

int msg_first_dex_pool_n(const uint8_t *payload, size_t len, uint32_t n_ids,
                         uint8_t *proto, const uint8_t **pool);

#endif /* ARB_CORE_TX_N_H */
