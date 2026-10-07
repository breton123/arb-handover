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

#define WARM_N     200
#define COLD_N     8
#define MAX_ROWS   256
#define CU_PRICE   ((uint64_t)10000)
#define POLL_MS    50
#define POLL_SEC   30
#define PACE_MS    350
#define MIN_WALLET 4000000ull

typedef struct {
    int      attempt;
    char     phase[8];
    char     sig[128];
    char     err[48];
    int      conn;
    int      landed;
    int      poll_n;
    uint64_t slot;
    uint64_t fee;
    uint64_t t0_ns;
    uint64_t t1_ns;
    uint64_t t2_ns;
    uint64_t t3_ns;
    uint64_t t_conf_ns;
    uint64_t t_fin_ns;
    uint64_t sign_ns;
    uint64_t send_ns;
    uint64_t sign_to_send_ns;
    uint64_t send_to_seen_ns;
    uint64_t sign_to_seen_ns;
} row_t;

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
    FILE *req;
    FILE *p;
    char cmd[768];
    size_t n;

    req = fopen("/tmp/swqos_bench_rpc.json", "w");
    if (req == NULL) {
        return -1;
    }
    fputs(body, req);
    fclose(req);
    snprintf(cmd, sizeof(cmd),
             "curl -sS --max-time 15 -X POST -H 'Content-Type: application/json' "
             "--data-binary @/tmp/swqos_bench_rpc.json '%s'", url);
    p = popen(cmd, "r");
    if (p == NULL) {
        return -1;
    }
    n = fread(out, 1, cap - 1, p);
    out[n] = '\0';
    if (pclose(p) != 0) {
        return -1;
    }
    return 0;
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
    if (json_quoted(resp, "blockhash", b58, sizeof(b58)) != 0) {
        return -1;
    }
    return b58_decode(b58, hash, 32);
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
rpc_fee(const char *url, const char *sig, uint64_t *fee)
{
    char body[768], resp[8192];

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\","
             "\"params\":[\"%s\",{\"encoding\":\"json\",\"maxSupportedTransactionVersion\":0}]}",
             sig);
    if (rpc_post(url, body, resp, sizeof(resp)) != 0) {
        return -1;
    }
    return json_u64(resp, "fee", fee);
}

static int
swqos_prepaid(uint64_t *lamports)
{
    const char *key;
    FILE *h;
    FILE *p;
    char resp[2048];
    size_t n;

    key = getenv("SWQOS_KEY");
    if (key == NULL) {
        key = getenv("SWQOS_API_KEY");
    }
    if (key == NULL) {
        return -1;
    }
    h = fopen("/tmp/swqos_bench_hdr", "w");
    if (h == NULL) {
        return -1;
    }
    fprintf(h, "Authorization: Bearer %s\n", key);
    fclose(h);
    p = popen("curl -sS --max-time 10 -H @/tmp/swqos_bench_hdr "
              "https://send.swqos.com/v1/account", "r");
    if (p == NULL) {
        (void)unlink("/tmp/swqos_bench_hdr");
        return -1;
    }
    n = fread(resp, 1, sizeof(resp) - 1, p);
    resp[n] = '\0';
    (void)pclose(p);
    (void)unlink("/tmp/swqos_bench_hdr");
    return json_u64(resp, "balance_lamports", lamports);
}

static int
poll_sig(const char *url, row_t *r)
{
    char body[640], resp[4096], status[32];
    uint64_t deadline, now;
    int seen = 0;

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getSignatureStatuses\","
             "\"params\":[[\"%s\"],{\"searchTransactionHistory\":true}]}",
             r->sig);
    deadline = now_ns() + (uint64_t)POLL_SEC * 1000000000ull;
    while ((now = now_ns()) < deadline) {
        r->poll_n++;
        if (rpc_post(url, body, resp, sizeof(resp)) != 0) {
            sleep_ms(POLL_MS);
            continue;
        }
        if (strstr(resp, "\"value\":[null]") != NULL ||
            strstr(resp, "\"value\":[ null") != NULL) {
            sleep_ms(POLL_MS);
            continue;
        }
        if (!seen && strstr(resp, "\"slot\":") != NULL) {
            r->t3_ns = now_ns();
            seen = 1;
            (void)json_u64(resp, "slot", &r->slot);
        }
        if (strstr(resp, "\"err\":{") != NULL ||
            strstr(resp, "\"err\": {") != NULL) {
            snprintf(r->err, sizeof(r->err), "tx_err");
            return seen ? 0 : -1;
        }
        if (json_quoted(resp, "confirmationStatus", status, sizeof(status)) == 0) {
            if (strcmp(status, "confirmed") == 0 ||
                strcmp(status, "finalized") == 0) {
                if (r->t_conf_ns == 0) {
                    r->t_conf_ns = now_ns();
                }
                r->landed = 1;
                if (strcmp(status, "finalized") == 0) {
                    r->t_fin_ns = now_ns();
                    return 0;
                }
                if (r->t_conf_ns != 0 && now_ns() > r->t_conf_ns + 8000000000ull) {
                    return 0;
                }
            }
        }
        sleep_ms(POLL_MS);
    }
    if (!seen) {
        snprintf(r->err, sizeof(r->err), "not_seen");
        return -1;
    }
    if (!r->landed) {
        snprintf(r->err, sizeof(r->err), "expired");
    }
    return 0;
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
    int i;

    if (n <= 0) {
        return 0;
    }
    qsort(v, (size_t)n, sizeof(uint64_t), cmp_u64);
    i = (int)(p * (double)(n - 1) + 0.5);
    if (i < 0) {
        i = 0;
    }
    if (i >= n) {
        i = n - 1;
    }
    return v[i];
}

static void
print_row(const char *name, uint64_t *v, int n)
{
    uint64_t tmp[MAX_ROWS];
    uint64_t mn, mx;
    int i;

    if (n <= 0) {
        printf("%-22s  n=0\n", name);
        return;
    }
    memcpy(tmp, v, (size_t)n * sizeof(uint64_t));
    mn = mx = tmp[0];
    for (i = 1; i < n; i++) {
        if (tmp[i] < mn) {
            mn = tmp[i];
        }
        if (tmp[i] > mx) {
            mx = tmp[i];
        }
    }
    printf("  %-22s %8.1f %8.1f %8.1f %8.1f %8.1f\n",
           name,
           (double)pct(tmp, n, 0.50) / 1000.0,
           (double)pct(tmp, n, 0.90) / 1000.0,
           (double)pct(tmp, n, 0.99) / 1000.0,
           (double)mn / 1000.0,
           (double)mx / 1000.0);
}

static void
write_files(const char *dir, const char *phase, const row_t *rows, int n)
{
    char path[256];
    FILE *csv, *js;
    int i;

    snprintf(path, sizeof(path), "%s/%s.csv", dir, phase);
    csv = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/%s.jsonl", dir, phase);
    js = fopen(path, "w");
    if (csv == NULL || js == NULL) {
        fprintf(stderr, "write %s\n", dir);
        return;
    }
    fputs("attempt,phase,signature,slot,conn,t0_ns,t1_ns,t2_ns,t3_ns,"
          "sign_ns,send_ns,sign_to_send_ns,send_to_seen_ns,sign_to_seen_ns,"
          "landed,err,rpc_poll_count,fee_lamports,t_confirmed_ns,t_finalized_ns\n",
          csv);
    for (i = 0; i < n; i++) {
        const row_t *r = &rows[i];
        fprintf(csv,
                "%d,%s,%s,%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%d,%s,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                r->attempt, r->phase, r->sig, r->slot, r->conn,
                r->t0_ns, r->t1_ns, r->t2_ns, r->t3_ns,
                r->sign_ns, r->send_ns, r->sign_to_send_ns,
                r->send_to_seen_ns, r->sign_to_seen_ns,
                r->landed, r->err, r->poll_n, r->fee,
                r->t_conf_ns, r->t_fin_ns);
        fprintf(js,
                "{\"attempt\":%d,\"phase\":\"%s\",\"signature\":\"%s\",\"slot\":%" PRIu64
                ",\"conn\":%d,\"t0_ns\":%" PRIu64 ",\"t1_ns\":%" PRIu64
                ",\"t2_ns\":%" PRIu64 ",\"t3_ns\":%" PRIu64
                ",\"sign_ns\":%" PRIu64 ",\"send_ns\":%" PRIu64
                ",\"sign_to_send_ns\":%" PRIu64 ",\"send_to_seen_ns\":%" PRIu64
                ",\"sign_to_seen_ns\":%" PRIu64 ",\"landed\":%s,\"err\":\"%s\""
                ",\"rpc_poll_count\":%d,\"fee_lamports\":%" PRIu64 "}\n",
                r->attempt, r->phase, r->sig, r->slot, r->conn,
                r->t0_ns, r->t1_ns, r->t2_ns, r->t3_ns,
                r->sign_ns, r->send_ns, r->sign_to_send_ns,
                r->send_to_seen_ns, r->sign_to_seen_ns,
                r->landed ? "true" : "false", r->err, r->poll_n, r->fee);
    }
    fclose(csv);
    fclose(js);
}

static void
summarize(const char *title, const row_t *rows, int n)
{
    uint64_t sign[MAX_ROWS], send[MAX_ROWS], s2s[MAX_ROWS];
    uint64_t seen[MAX_ROWS], s2seen[MAX_ROWS];
    int ns = 0, nd = 0, nseen = 0, land = 0, i;
    int e_send = 0, e_sign = 0, e_seen = 0, e_exp = 0, e_tx = 0, e_other = 0;
    int c0 = 0, c1 = 0;

    printf("\n%s  n=%d\n", title, n);
    printf("                     p50      p90      p99      min      max   (µs)\n");
    for (i = 0; i < n; i++) {
        if (rows[i].sign_ns > 0) {
            sign[ns++] = rows[i].sign_ns;
        }
        if (rows[i].send_ns > 0 && strcmp(rows[i].err, "send_fail") != 0) {
            send[nd] = rows[i].send_ns;
            s2s[nd] = rows[i].sign_to_send_ns;
            nd++;
        }
        if (rows[i].t3_ns > rows[i].t2_ns) {
            seen[nseen] = rows[i].send_to_seen_ns;
            s2seen[nseen] = rows[i].sign_to_seen_ns;
            nseen++;
        }
        if (rows[i].landed) {
            land++;
        }
        if (rows[i].conn == 0) {
            c0++;
        } else if (rows[i].conn == 1) {
            c1++;
        }
        if (strcmp(rows[i].err, "send_fail") == 0) {
            e_send++;
        } else if (strcmp(rows[i].err, "sign_fail") == 0) {
            e_sign++;
        } else if (strcmp(rows[i].err, "not_seen") == 0) {
            e_seen++;
        } else if (strcmp(rows[i].err, "expired") == 0) {
            e_exp++;
        } else if (strcmp(rows[i].err, "tx_err") == 0) {
            e_tx++;
        } else if (rows[i].err[0] != '\0' && strcmp(rows[i].err, "-") != 0) {
            e_other++;
        }
    }
    print_row("sign", sign, ns);
    print_row("signed→SWQOS return", send, nd);
    print_row("sign→SWQOS return", s2s, nd);
    print_row("SWQOS return→RPC seen", seen, nseen);
    print_row("sign→RPC seen", s2seen, nseen);
    printf("  sent %d  landed %d  not_landed %d  landing %.1f%%\n",
           nd, land, nd - land, nd ? 100.0 * (double)land / (double)nd : 0.0);
    printf("  conn0 %d  conn1 %d\n", c0, c1);
    printf("  errors sign=%d send=%d not_seen=%d expired=%d tx_err=%d other=%d\n",
           e_sign, e_send, e_seen, e_exp, e_tx, e_other);
}

static int
one_attempt(const char *rpc, const route0_signer_t *wallet,
            const char *phase, int attempt, int reconnect, row_t *r)
{
    smoke_tx_t tx;
    uint8_t bh[32];
    int rc;

    memset(r, 0, sizeof(*r));
    r->attempt = attempt;
    snprintf(r->phase, sizeof(r->phase), "%s", phase);
    snprintf(r->err, sizeof(r->err), "-");
    r->conn = -1;

    if (reconnect) {
        swqos_close();
        if (swqos_open_env() != 0) {
            snprintf(r->err, sizeof(r->err), "open_fail");
            return -1;
        }
    }
    if (rpc_blockhash(rpc, bh) != 0) {
        snprintf(r->err, sizeof(r->err), "blockhash");
        return -1;
    }
    if (smoke_build_memo(wallet->pk, bh, CU_PRICE, &tx) != 0) {
        snprintf(r->err, sizeof(r->err), "build");
        return -1;
    }

    r->t0_ns = now_ns();
    (void)rdtscp();
    rc = exec_sign(wallet, tx.bytes + 1u, tx.bytes + tx.msg_off, tx.msg_len);
    r->t1_ns = now_ns();
    if (rc != 0) {
        snprintf(r->err, sizeof(r->err), "sign_fail");
        return -1;
    }
    if (b58_encode(tx.bytes + 1u, 64, r->sig, sizeof(r->sig)) != 0) {
        snprintf(r->err, sizeof(r->err), "b58");
        return -1;
    }

    rc = swqos_send(tx.bytes, tx.len);
    r->t2_ns = now_ns();
    r->conn = swqos_last_conn();
    r->sign_ns = r->t1_ns - r->t0_ns;
    r->send_ns = r->t2_ns - r->t1_ns;
    r->sign_to_send_ns = r->t2_ns - r->t0_ns;
    if (rc != 0) {
        snprintf(r->err, sizeof(r->err), "send_fail");
        return -1;
    }
    (void)poll_sig(rpc, r);
    if (r->t3_ns > r->t2_ns) {
        r->send_to_seen_ns = r->t3_ns - r->t2_ns;
        r->sign_to_seen_ns = r->t3_ns - r->t0_ns;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    const char *rpc, *priv, *pub;
    char want_b58[128];
    uint8_t secret[64], want_pk[32];
    route0_signer_t wallet;
    row_t warm[WARM_N];
    row_t cold[COLD_N];
    const char *out = "/home/louis/arb-cap/swqos_bench";
    uint64_t bal0 = 0, bal1 = 0, sw0 = 0, sw1 = 0, fees = 0;
    struct tsc_clock tsc;
    int i, nwarm = 0, ncold = 0, warm_target = WARM_N, cold_target = COLD_N;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--warm") == 0 && i + 1 < argc) {
            warm_target = atoi(argv[++i]);
            if (warm_target > WARM_N) {
                warm_target = WARM_N;
            }
        } else if (strcmp(argv[i], "--cold") == 0 && i + 1 < argc) {
            cold_target = atoi(argv[++i]);
            if (cold_target > COLD_N) {
                cold_target = COLD_N;
            }
        }
    }

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
    if (b58_decode(pub, want_pk, 32) != 0) {
        fprintf(stderr, "bad PUBLIC_KEY\n");
        return 1;
    }
    if (route0_signer_init(&wallet, secret) != 0 ||
        memcmp(wallet.pk, want_pk, 32) != 0) {
        fprintf(stderr, "signer mismatch\n");
        return 1;
    }
    memset(secret, 0, sizeof(secret));
    if (b58_encode(wallet.pk, 32, want_b58, sizeof(want_b58)) != 0) {
        return 1;
    }
    if (tsc_calibrate(&tsc) != 0) {
        fprintf(stderr, "tsc\n");
        return 1;
    }
    if (rpc_balance(rpc, pub, &bal0) != 0 || bal0 < MIN_WALLET) {
        fprintf(stderr, "wallet too low\n");
        return 1;
    }
    (void)swqos_prepaid(&sw0);
    printf("SWQOS-BENCH  payer %s  wallet %" PRIu64 "  swqos %" PRIu64
           "  tsc_hz %.0f\n",
           want_b58, bal0, sw0, tsc.hz);
    printf("  memo-only  cu_price=%" PRIu64 "  warm=%d  cold=%d\n",
           CU_PRICE, warm_target, cold_target);

    if (swqos_open_env() != 0) {
        fprintf(stderr, "swqos_open_env\n");
        return 1;
    }

    for (i = 0; i < warm_target; i++) {
        (void)one_attempt(rpc, &wallet, "warm", i, 0, &warm[nwarm]);
        printf("  warm %d  send=%.1fµs  seen=%s  land=%d\n",
               i,
               (double)warm[nwarm].send_ns / 1000.0,
               warm[nwarm].t3_ns ? "y" : "n",
               warm[nwarm].landed);
        nwarm++;
        sleep_ms(PACE_MS);
    }

    printf("COLD CONTROL  reconnect per send\n");
    for (i = 0; i < cold_target; i++) {
        (void)one_attempt(rpc, &wallet, "cold", i, 1, &cold[ncold]);
        printf("  cold %d  send=%.1fµs  seen=%s  land=%d\n",
               i,
               (double)cold[ncold].send_ns / 1000.0,
               cold[ncold].t3_ns ? "y" : "n",
               cold[ncold].landed);
        ncold++;
        sleep_ms(PACE_MS);
    }
    swqos_close();

    for (i = 0; i < nwarm; i++) {
        if (warm[i].landed && warm[i].sig[0] &&
            rpc_fee(rpc, warm[i].sig, &warm[i].fee) == 0) {
            fees += warm[i].fee;
        }
        sleep_ms(40);
    }
    for (i = 0; i < ncold; i++) {
        if (cold[i].landed && cold[i].sig[0] &&
            rpc_fee(rpc, cold[i].sig, &cold[i].fee) == 0) {
            fees += cold[i].fee;
        }
    }
    (void)rpc_balance(rpc, pub, &bal1);
    (void)swqos_prepaid(&sw1);

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", out);
        if (system(cmd) != 0) {
            fprintf(stderr, "mkdir %s\n", out);
        }
    }
    write_files(out, "warm", warm, nwarm);
    write_files(out, "cold", cold, ncold);
    summarize("WARM", warm, nwarm);
    summarize("COLD CONTROL", cold, ncold);
    printf("  wallet Δ %" PRId64 " lamports  (%.6f SOL)\n",
           (int64_t)bal1 - (int64_t)bal0,
           (double)((int64_t)bal1 - (int64_t)bal0) / 1e9);
    printf("  swqos prepaid Δ %" PRId64 " lamports  (%.6f SOL)\n",
           (int64_t)sw1 - (int64_t)sw0,
           (double)((int64_t)sw1 - (int64_t)sw0) / 1e9);
    printf("  recovered tx fees %" PRIu64 " lamports\n", fees);
    printf("  wrote %s/warm.jsonl %s/cold.jsonl\n", out, out);
    return 0;
}
