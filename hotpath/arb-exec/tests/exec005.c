#include "leader.h"
#include "nonce.h"
#include "sign.h"
#include "tx_template.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL  %s\n", (msg)); \
            g_fail++;                             \
        } else {                                  \
            printf("ok    %s\n", (msg));          \
        }                                         \
    } while (0)

static void
set32(uint8_t *p, uint8_t tag, uint32_t n)
{
    memset(p, 0, 32);
    p[0] = tag;
    p[1] = (uint8_t)n;
    p[2] = (uint8_t)(n >> 8);
}

static void
fill_keys(uint8_t keys[ROUTE0_N][32], uint8_t our_exec[32])
{
    uint32_t i;

    memset(keys, 0, sizeof(uint8_t) * ROUTE0_N * 32);
    for (i = 0; i < ROUTE0_N; i++) {
        keys[i][0] = 0xA0;
        keys[i][1] = (uint8_t)i;
    }
    memcpy(keys[ROUTE0_ACC_PUMP_BASE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_X_MINT], 32);
    memcpy(keys[ROUTE0_ACC_PUMP_QUOTE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_Y_MINT], 32);
    memset(our_exec, 0, 32);
    our_exec[0] = 0xE0;
}

static void
fill_pool(nonce_pool_t *p)
{
    uint32_t i;
    uint8_t pk[32], h[32];

    nonce_pool_init(p);
    for (i = 0; i < NONCE_POOL_N; i++) {
        set32(pk, 0xB0, i);
        set32(h, 0xC0, i);
        if (nonce_load(p, i, pk, h) != 0) {
            abort();
        }
    }
}

static void
mk_opp(opportunity_t *o, uint64_t amount_in, uint8_t dir)
{
    memset(o, 0, sizeof(*o));
    o->route_id     = ROUTE_DLMM_PUMP;
    o->amount_in    = amount_in;
    o->amount_out   = amount_in + 1u;
    o->gross_profit = 1;
    o->direction    = dir;
    o->valid        = 1;
}

static int
sink_open(uint16_t *port)
{
    struct sockaddr_in addr;
    struct timeval tv;
    socklen_t alen;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(fd);
        return -1;
    }
    memset(&tv, 0, sizeof(tv));
    tv.tv_sec = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

static int
fire(const route0_tx_template_t *tmpl,
     nonce_pool_t *pool,
     const route0_signer_t *signer,
     leader_conn_t *leader,
     const opportunity_t *opp,
     uint64_t min_profit,
     uint64_t cu_price,
     uint8_t *tx,
     nonce_claim_t *claim)
{
    if (nonce_claim(pool, claim) != 0) {
        return -1;
    }
    if (route0_tx_patch(tmpl, opp, min_profit, claim->hash, cu_price, tx) != 0) {
        return -1;
    }
    if (route0_sign(signer, tx) != 0) {
        return -1;
    }
    return leader_send(leader, tx, ROUTE0_TX_LEN);
}

int
main(void)
{
    leader_conn_t leaders[LEADER_N];
    route0_tx_template_t tmpl;
    nonce_pool_t pool;
    route0_signer_t signer;
    opportunity_t opp;
    nonce_claim_t ca, cb;
    uint8_t keys[ROUTE0_N][32];
    uint8_t our_exec[32];
    uint8_t seed[32];
    uint8_t txa[ROUTE0_TX_LEN], txb[ROUTE0_TX_LEN];
    uint8_t got[ROUTE0_TX_LEN];
    uint16_t port = 0;
    int sink;
    uint32_t i;
    ssize_t n;
    uint64_t amt;

    for (i = 0; i < LEADER_N; i++) {
        leaders[i].fd = -1;
    }

    sink = sink_open(&port);
    CHECK(sink >= 0 && port != 0, "local sink");
    CHECK(leader_conn_open(&leaders[0], "127.0.0.1", port) == 0,
          "open leaders[0]");
    CHECK(leaders[0].fd >= 0, "leaders[0] ready");

    fill_keys(keys, our_exec);
    CHECK(route0_tx_compile(&keys[0][0], our_exec, ROUTE0_CU_LIMIT, &tmpl) == 0,
          "template");
    fill_pool(&pool);
    if (getrandom(seed, sizeof(seed), 0) != (ssize_t)sizeof(seed)) {
        fprintf(stderr, "FAIL  getrandom\n");
        return 1;
    }
    CHECK(route0_signer_init(&signer, seed) == 0, "signer");
    memset(seed, 0, sizeof(seed));

    mk_opp(&opp, 111111ull, 0);
    CHECK(fire(&tmpl, &pool, &signer, &leaders[0], &opp, 7, 1000, txa, &ca) == 0,
          "opportunity → nonce → patch → sign → leader_send");
    n = recv(sink, got, sizeof(got), 0);
    CHECK(n == (ssize_t)ROUTE0_TX_LEN, "sink received 1305 bytes");
    CHECK(memcmp(got, txa, ROUTE0_TX_LEN) == 0, "wire bytes match signed tx");
    CHECK(memcmp(got + ROUTE0_OFF_BLOCKHASH, ca.hash, 32) == 0,
          "nonce hash on the wire");
    memcpy(&amt, got + ROUTE0_OFF_AMOUNT_IN, 8);
    CHECK(amt == 111111ull, "amount_in on the wire");
    CHECK(route0_verify_sodium(got, signer.pk) == 0, "wire signature verifies");

    mk_opp(&opp, 222222ull, 1);
    CHECK(fire(&tmpl, &pool, &signer, &leaders[0], &opp, 7, 2000, txb, &cb) == 0,
          "second attempt");
    CHECK(ca.idx != cb.idx, "attempts used distinct nonces");
    n = recv(sink, got, sizeof(got), 0);
    CHECK(n == (ssize_t)ROUTE0_TX_LEN, "sink received attempt B");
    CHECK(memcmp(got + ROUTE0_OFF_BLOCKHASH, txa + ROUTE0_OFF_BLOCKHASH, 32) != 0,
          "attempts do not collide on the wire");

    CHECK(leader_send(&leaders[1], txa, ROUTE0_TX_LEN) != 0,
          "unopened leader fails closed");

    leader_conn_close(&leaders[0]);
    close(sink);

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nEXEC-005 ok  path ends at leader_send()\n");
    return 0;
}
