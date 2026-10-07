#include "route0_v0.h"
#include "tx_template.h"

#include <stdio.h>
#include <string.h>
#include <sys/random.h>

int crypto_sign_ed25519_verify_detached(const unsigned char *sig,
                                        const unsigned char *m,
                                        unsigned long long mlen,
                                        const unsigned char *pk);

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
}

int
main(void)
{
    uint8_t sk[V0_N_STATIC][32];
    uint8_t wr[V0_N_ALT_WR][32];
    uint8_t ro[V0_N_ALT_RO][32];
    uint8_t alt[32], seed[32], blockhash[32];
    uint8_t tx[V0_TX_LEN], tx2[V0_TX_LEN];
    route0_v0_template_t tmpl;
    route0_signer_t signer;
    opportunity_t opp;
    uint32_t i;
    uint64_t v;

    memset(sk, 0, sizeof(sk));
    memset(wr, 0, sizeof(wr));
    memset(ro, 0, sizeof(ro));
    memset(alt, 0, sizeof(alt));
    alt[0] = 0xA1;
    for (i = 0; i < V0_N_STATIC; i++) {
        set32(sk[i], 0x50, i);
    }
    for (i = 0; i < V0_N_ALT_WR; i++) {
        set32(wr[i], 0x51, i);
    }
    for (i = 0; i < V0_N_ALT_RO; i++) {
        set32(ro[i], 0x52, i);
    }

    CHECK(route0_v0_compile(&sk[0][0], alt, &wr[0][0], &ro[0][0],
                            ROUTE0_CU_LIMIT, &tmpl) == 0,
          "v0 message compiles");
    CHECK(tmpl.bytes[0] == 1 && tmpl.bytes[65] == 0x80, "v0 prefix");
    CHECK(tmpl.bytes[66] == 1 && tmpl.bytes[67] == 0 && tmpl.bytes[68] == 10,
          "header 1 / 0 / 10 ro unsigned");
    CHECK(tmpl.bytes[69] == (uint8_t)V0_N_STATIC, "11 static keys");
    CHECK(V0_TX_LEN == 619u && V0_TX_LEN < 1100u, "619 B, margin under 1232");

    memset(&opp, 0, sizeof(opp));
    opp.route_id  = ROUTE_DLMM_PUMP;
    opp.amount_in = 10000;
    opp.valid     = 1;
    set32(blockhash, 0xBB, 1);
    CHECK(route0_v0_patch(&tmpl, &opp, 1000000000ull, blockhash, 7, tx) == 0,
          "patch");
    CHECK(memcmp(tx + V0_OFF_BLOCKHASH, blockhash, 32) == 0, "blockhash slot");
    memcpy(&v, tx + V0_OFF_AMOUNT_IN, 8);
    CHECK(v == 10000ull, "amount_in");
    memcpy(&v, tx + V0_OFF_MIN_PROFIT, 8);
    CHECK(v == 1000000000ull, "min_profit");
    memcpy(&v, tx + V0_OFF_CU_PRICE, 8);
    CHECK(v == 7ull, "cu_price");
    CHECK(tx[V0_OFF_DIRECTION] == 0, "direction");
    CHECK(tx[536] == 0x41 && tx[537] == 0x52 && tx[538] == 0x42, "ARBEXEC0");
    CHECK(tx[475 + 2] == 0 && tx[475 + 12] == 7 && tx[475 + 16] == 7,
          "logical 0 / bitmap / host map (optional none = DLMM)");

    if (getrandom(seed, 32, 0) != 32) {
        fprintf(stderr, "FAIL  getrandom\n");
        return 1;
    }
    CHECK(route0_signer_init(&signer, seed) == 0, "signer");
    memset(seed, 0, sizeof(seed));
    CHECK(route0_v0_sign(&signer, tx) == 0, "libsodium signs v0 slice");
    CHECK(crypto_sign_ed25519_verify_detached(
              tx + V0_OFF_SIGNATURE, tx + V0_OFF_MESSAGE, V0_MSG_LEN,
              signer.pk) == 0,
          "v0 message slice verifies");

    memcpy(tx2, tx, V0_TX_LEN);
    tx2[V0_OFF_AMOUNT_IN] ^= 1;
    CHECK(crypto_sign_ed25519_verify_detached(
              tx2 + V0_OFF_SIGNATURE, tx2 + V0_OFF_MESSAGE, V0_MSG_LEN,
              signer.pk) != 0,
          "tamper fails");

    if (g_fail != 0) {
        fprintf(stderr, "\n%d failed\n", g_fail);
        return 1;
    }
    printf("\nEXEC-LIVE-002B  v0 compile+sign ok  %u B  (1232-1100 margin)\n",
           V0_TX_LEN);
    return 0;
}
