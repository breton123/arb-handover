#ifndef ARB_EXEC_SWQOS_H
#define ARB_EXEC_SWQOS_H

#include <stdint.h>

/*
 * Live last hop. Frozen leader_send() stays the local-loopback proof.
 *
 * Control plane: swqos_open() holds a READY pool (4 QUIC conns) to
 * send.swqos.com:11000 (ALPN ultrasend/1). A control thread reconnects
 * DEAD slots. Hot path never DNS, handshake, or reconnects.
 *
 * Hot: pick READY → open stream → write → finish.
 * Fail: mark DEAD, try one other READY, then SEND_FAIL.
 */

#define SWQOS_TX_MAX  1232u
#define SWQOS_POOL_N  4

int swqos_open(const char *api_key);
int swqos_open_env(void);
int swqos_send(const uint8_t *tx, uint16_t len);
int swqos_last_conn(void);
int swqos_ready_n(void);
void swqos_close(void);

#endif /* ARB_EXEC_SWQOS_H */
