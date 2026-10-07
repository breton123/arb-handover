#include "b58.h"
#include "sign.h"
#include "smoke_tx.h"
#include "swqos.h"
#include "tsc.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WARM_N     100
#define CU_PRICE   ((uint64_t)10000)
#define PACE_MS    80
#define MIN_WALLET 4000000ull

static void
sleep_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

static void
load_kv(const char *path)
{
    FILE *f;
    char line[2048];

    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq, *k, *v, *nl;
        nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        nl = strchr(line, '\r');
        if (nl != NULL) {
            *nl = '\0';
        }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        k = strncmp(line, "export ", 7) == 0 ? line + 7 : line;
        eq = strchr(k, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        v = eq + 1;
        if ((v[0] == '"' || v[0] == '\'') && v[strlen(v) - 1] == v[0]) {
            v[strlen(v) - 1] = '\0';
            v++;
        }
        if (getenv(k) == NULL) {
            setenv(k, v, 0);
        }
        memset(eq + 1, 0, strlen(eq + 1));
    }
    fclose(f);
}

static int
rpc_post(const char *url, const char *body, char *out, size_t cap)
{
    FILE *req, *p;
    char cmd[768];
    size_t n;

    req = fopen("/tmp/swqos_v1_rpc.json", "w");
    if (req == NULL) {
        return -1;
    }
    fputs(body, req);
    fclose(req);
    snprintf(cmd, sizeof(cmd),
             "curl -sS --max-time 15 -X POST -H 'Content-Type: application/json' "
             "--data-binary @/tmp/swqos_v1_rpc.json '%s'", url);
    p = popen(cmd, "r");
    if (p == NULL) {
        return -1;
    }
    n = fread(out, 1, cap - 1, p);
    out[n] = '\0';
    return pclose(p) == 0 ? 0 : -1;
}

static int
json_quoted(const char *buf, const char *key, char *out, size_t cap)
{
    char pat[96];
    const char *p, *q;

    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(buf, pat);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pat);
    q = strchr(p, '"');
    if (q == NULL || (size_t)(q - p) >= cap) {
        return -1;
    }
    memcpy(out, p, (size_t)(q - p));
    out[q - p] = '\0';
    return 0;
}

static int
json_u64(const char *buf, const char *key, uint64_t *out)
{
    char pat[96];
    const char *p;
    char *end;

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(buf, pat);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pat);
    *out = strtoull(p, &end, 10);
    return end == p ? -1 : 0;
}

static int
rpc_blockhash(const char *url, uint8_t hash[32])
{
    char resp[4096], b58[128];

    if (rpc_post(url,
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getLatestBlockhash\","
                 "\"params\":[{\"commitment\":\"confirmed\"}]}",
                 resp, sizeof(resp)) != 0) {
        return -1;
    }
    return json_quoted(resp, "blockhash", b58, sizeof(b58)) == 0
        ? b58_decode(b58, hash, 32) : -1;
}

static int
rpc_balance(const char *url, const char *pub, uint64_t *lamports)
{
    char body[512], resp[2048];

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBalance\","
             "\"params\":[\"%s\"]}", pub);
    if (rpc_post(url, body, resp, sizeof(resp)) != 0) {
        return -1;
    }
    return json_u64(resp, "value", lamports);
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t
pct(uint64_t *v, int n, double p)
{
    uint64_t tmp[WARM_N];
    int i;

    if (n <= 0) {
        return 0;
    }
    memcpy(tmp, v, (size_t)n * sizeof(uint64_t));
    qsort(tmp, (size_t)n, sizeof(uint64_t), cmp_u64);
    i = (int)(p * (double)(n - 1) + 0.5);
    if (i < 0) {
        i = 0;
    }
    if (i >= n) {
        i = n - 1;
    }
    return tmp[i];
}

static int
one_send(const char *rpc, const route0_signer_t *wallet,
         uint64_t *sign_ns, uint64_t *send_ns, int *conn, char *err, size_t err_cap)
{
    smoke_tx_t tx;
    uint8_t bh[32];
    uint64_t t0, t1, t2;
    int rc;

    snprintf(err, err_cap, "-");
    if (rpc_blockhash(rpc, bh) != 0) {
        snprintf(err, err_cap, "blockhash");
        return -1;
    }
    if (smoke_build_memo(wallet->pk, bh, CU_PRICE, &tx) != 0) {
        snprintf(err, err_cap, "build");
        return -1;
    }
    t0 = now_ns();
    (void)rdtscp();
    rc = exec_sign(wallet, tx.bytes + 1u, tx.bytes + tx.msg_off, tx.msg_len);
    t1 = now_ns();
    if (rc != 0) {
        snprintf(err, err_cap, "sign_fail");
        return -1;
    }
    rc = swqos_send(tx.bytes, tx.len);
    t2 = now_ns();
    *sign_ns = t1 - t0;
    *send_ns = t2 - t1;
    *conn = swqos_last_conn();
    if (rc != 0) {
        snprintf(err, err_cap, "send_fail");
        return -1;
    }
    return 0;
}

int
main(void)
{
    const char *rpc, *priv, *pub;
    uint8_t secret[64], want_pk[32];
    route0_signer_t wallet;
    uint64_t sign[WARM_N], send[WARM_N], bal = 0;
    char err[48];
    int i, conn, ok = 0, fail = 0;
    int idle_sec[] = { 1, 10, 30, 60, 300 };
    int idle_n = (int)(sizeof(idle_sec) / sizeof(idle_sec[0]));
    FILE *js;

    load_kv(".env");
    load_kv("../.env");
    load_kv("/home/louis/.arb-smoke.env");
    load_kv("/home/louis/.arb-swqos.env");
    load_kv("/home/louis/arb-exec/.deploy/swqos.env");

    rpc = getenv("RPC_URL");
    if (rpc == NULL) {
        rpc = getenv("HELIUS_RPC_URL");
    }
    priv = getenv("PRIVATE_KEY");
    pub = getenv("PUBLIC_KEY");
    if (rpc == NULL || priv == NULL || pub == NULL) {
        fprintf(stderr, "need RPC_URL PRIVATE_KEY PUBLIC_KEY\n");
        return 1;
    }
    if (b58_decode(priv, secret, 64) != 0 &&
        b58_decode(priv, secret, 32) != 0) {
        fprintf(stderr, "bad PRIVATE_KEY\n");
        return 1;
    }
    if (b58_decode(pub, want_pk, 32) != 0 ||
        route0_signer_init(&wallet, secret) != 0 ||
        memcmp(wallet.pk, want_pk, 32) != 0) {
        fprintf(stderr, "signer mismatch\n");
        return 1;
    }
    memset(secret, 0, sizeof(secret));
    if (rpc_balance(rpc, pub, &bal) != 0 || bal < MIN_WALLET) {
        fprintf(stderr, "wallet too low\n");
        return 1;
    }
    if (swqos_open_env() != 0) {
        fprintf(stderr, "swqos_open\n");
        return 1;
    }
    for (i = 0; i < 40 && swqos_ready_n() < 2; i++) {
        sleep_ms(50);
    }
    printf("SWQOS-V1  ready=%d/%d  wallet %" PRIu64 "  warm=%d\n",
           swqos_ready_n(), SWQOS_POOL_N, bal, WARM_N);
    if (swqos_ready_n() < 2) {
        fprintf(stderr, "pool not ready\n");
        swqos_close();
        return 1;
    }

    if (system("mkdir -p /home/louis/arb-cap/swqos_bench") != 0) {
        fprintf(stderr, "mkdir\n");
    }
    js = fopen("/home/louis/arb-cap/swqos_bench/v1.jsonl", "w");
    if (js == NULL) {
        swqos_close();
        return 1;
    }

    for (i = 0; i < WARM_N; i++) {
        uint64_t sn = 0, sd = 0;
        int rc = one_send(rpc, &wallet, &sn, &sd, &conn, err, sizeof(err));
        if (rc == 0) {
            sign[ok] = sn;
            send[ok] = sd;
            ok++;
        } else {
            fail++;
        }
        fprintf(js,
                "{\"phase\":\"warm\",\"i\":%d,\"ok\":%s,\"send_ns\":%" PRIu64
                ",\"sign_ns\":%" PRIu64 ",\"conn\":%d,\"ready\":%d,\"err\":\"%s\"}\n",
                i, rc == 0 ? "true" : "false", sd, sn, conn, swqos_ready_n(), err);
        printf("  warm %d  send=%.1fµs  ready=%d  %s\n",
               i, (double)sd / 1000.0, swqos_ready_n(), err);
        sleep_ms(PACE_MS);
    }

    printf("\nWARM  ok=%d fail=%d\n", ok, fail);
    if (ok > 0) {
        printf("                     p50      p90      p99      min      max  (µs)\n");
        printf("  sign               %7.1f  %7.1f  %7.1f  %7.1f  %7.1f\n",
               (double)pct(sign, ok, 0.50) / 1000.0,
               (double)pct(sign, ok, 0.90) / 1000.0,
               (double)pct(sign, ok, 0.99) / 1000.0,
               (double)pct(sign, ok, 0.00) / 1000.0,
               (double)pct(sign, ok, 1.00) / 1000.0);
        printf("  signed→SWQOS ret   %7.1f  %7.1f  %7.1f  %7.1f  %7.1f\n",
               (double)pct(send, ok, 0.50) / 1000.0,
               (double)pct(send, ok, 0.90) / 1000.0,
               (double)pct(send, ok, 0.99) / 1000.0,
               (double)pct(send, ok, 0.00) / 1000.0,
               (double)pct(send, ok, 1.00) / 1000.0);
    }

    printf("\nIDLE  hold pool, one send after each gap\n");
    for (i = 0; i < idle_n; i++) {
        uint64_t sn = 0, sd = 0;
        printf("  sleep %ds  ready=%d\n", idle_sec[i], swqos_ready_n());
        sleep(idle_sec[i]);
        if (one_send(rpc, &wallet, &sn, &sd, &conn, err, sizeof(err)) == 0) {
            printf("  idle %ds  send=%.1fµs  ready=%d  ok\n",
                   idle_sec[i], (double)sd / 1000.0, swqos_ready_n());
            fprintf(js,
                    "{\"phase\":\"idle\",\"idle_s\":%d,\"ok\":true,\"send_ns\":%" PRIu64
                    ",\"sign_ns\":%" PRIu64 ",\"conn\":%d,\"ready\":%d}\n",
                    idle_sec[i], sd, sn, conn, swqos_ready_n());
        } else {
            printf("  idle %ds  FAIL %s  ready=%d\n", idle_sec[i], err, swqos_ready_n());
            fprintf(js,
                    "{\"phase\":\"idle\",\"idle_s\":%d,\"ok\":false,\"err\":\"%s\",\"ready\":%d}\n",
                    idle_sec[i], err, swqos_ready_n());
        }
    }
    fclose(js);
    swqos_close();
    if (ok < WARM_N) {
        printf("V1 FAIL  need %d send successes, got %d\n", WARM_N, ok);
        return 2;
    }
    printf("V1 PASS  %d/%d swqos_send on READY pool, no hot reconnect\n", ok, WARM_N);
    return 0;
}
