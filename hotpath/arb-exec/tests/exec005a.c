#include "b58.h"
#include "leader.h"
#include "sign.h"
#include "smoke_tx.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#define CU_PRICE  10000ull
#define POLL_SEC  45

static int
load_env(const char *path, char *rpc, size_t rpc_cap,
         char *priv, size_t priv_cap, char *pub, size_t pub_cap)
{
    FILE *f;
    char line[2048];

    rpc[0] = priv[0] = pub[0] = '\0';
    f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *nl = strchr(line, '\n');
        char *cr = strchr(line, '\r');
        char *eq;
        if (nl != NULL) {
            *nl = '\0';
        }
        if (cr != NULL) {
            *cr = '\0';
        }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        if (strcmp(line, "RPC_URL") == 0) {
            snprintf(rpc, rpc_cap, "%s", eq + 1);
        } else if (strcmp(line, "PRIVATE_KEY") == 0) {
            snprintf(priv, priv_cap, "%s", eq + 1);
        } else if (strcmp(line, "PUBLIC_KEY") == 0) {
            snprintf(pub, pub_cap, "%s", eq + 1);
        }
    }
    fclose(f);
    return (rpc[0] && priv[0] && pub[0]) ? 0 : -1;
}

static int
rpc_post(const char *url, const char *body, char *out, size_t cap)
{
    FILE *req;
    FILE *p;
    char cmd[768];
    size_t n;

    req = fopen("/tmp/arb-rpc-req.json", "w");
    if (req == NULL) {
        return -1;
    }
    fputs(body, req);
    fclose(req);
    snprintf(cmd, sizeof(cmd),
             "curl -sS --max-time 20 -X POST -H 'Content-Type: application/json' "
             "--data-binary @/tmp/arb-rpc-req.json '%s'", url);
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
        fprintf(stderr, "blockhash parse\n");
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
rpc_rent(const char *url, uint64_t space, uint64_t *lamports)
{
    char body[256], resp[2048];

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getMinimumBalanceForRentExemption\","
             "\"params\":[%" PRIu64 "]}", space);
    if (rpc_post(url, body, resp, sizeof(resp)) != 0) {
        return -1;
    }
    return json_u64(resp, "result", lamports);
}

static int
rpc_send(const char *url, const uint8_t *tx, uint16_t len, char *sig, size_t sig_cap)
{
    char b64[2048], body[4096], resp[4096];

    if (b64_encode(tx, len, b64, sizeof(b64)) != 0) {
        return -1;
    }
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendTransaction\","
             "\"params\":[\"%s\",{\"encoding\":\"base64\",\"skipPreflight\":false,"
             "\"preflightCommitment\":\"confirmed\"}]}", b64);
    if (rpc_post(url, body, resp, sizeof(resp)) != 0) {
        return -1;
    }
    if (strstr(resp, "\"error\"") != NULL) {
        fprintf(stderr, "sendTransaction error: %.400s\n", resp);
        return -1;
    }
    return json_quoted(resp, "result", sig, sig_cap);
}

static int
rpc_wait(const char *url, const char *sig, uint64_t *slot)
{
    char body[512], resp[4096], status[32];
    int i;

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getSignatureStatuses\","
             "\"params\":[[\"%s\"],{\"searchTransactionHistory\":true}]}", sig);
    for (i = 0; i < POLL_SEC; i++) {
        if (rpc_post(url, body, resp, sizeof(resp)) == 0 &&
            json_quoted(resp, "confirmationStatus", status, sizeof(status)) == 0) {
            if (strcmp(status, "confirmed") == 0 ||
                strcmp(status, "finalized") == 0) {
                if (json_u64(resp, "slot", slot) != 0) {
                    *slot = 0;
                }
                return 0;
            }
        }
        sleep(1);
    }
    fprintf(stderr, "not confirmed: %.300s\n", resp);
    return -1;
}

static int
rpc_nonce_hash(const char *url, const char *nonce_b58, uint8_t hash[32])
{
    char body[640], resp[8192], bh[128];

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccountInfo\","
             "\"params\":[\"%s\",{\"encoding\":\"jsonParsed\","
             "\"commitment\":\"confirmed\"}]}", nonce_b58);
    if (rpc_post(url, body, resp, sizeof(resp)) != 0) {
        return -1;
    }
    if (json_quoted(resp, "blockhash", bh, sizeof(bh)) != 0) {
        fprintf(stderr, "nonce parse\n");
        return -1;
    }
    return b58_decode(bh, hash, 32);
}

static int
send_and_confirm(const char *url, smoke_tx_t *tx, const char *label)
{
    char sig[128];
    uint64_t slot = 0;

    if (rpc_send(url, tx->bytes, tx->len, sig, sizeof(sig)) != 0) {
        return -1;
    }
    printf("  %s sig  %s\n", label, sig);
    if (rpc_wait(url, sig, &slot) != 0) {
        return -1;
    }
    printf("  %s slot %" PRIu64 "  CONFIRMED\n", label, slot);
    return 0;
}

int
main(int argc, char **argv)
{
    const char *envpath;
    char rpc[512], priv_b58[256], pub_b58[128], want_b58[128];
    uint8_t secret[64], want_pk[32], bh[32];
    route0_signer_t wallet;
    smoke_tx_t tx;
    uint64_t bal = 0, rent = 0;
    int do_nonce = 1;
    const char *advance_acc = NULL;
    int i;

    envpath = getenv("SMOKE_ENV");
    if (envpath == NULL) {
        envpath = "/home/louis/.arb-smoke.env";
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-nonce") == 0) {
            do_nonce = 0;
        } else if (strcmp(argv[i], "--advance") == 0 && i + 1 < argc) {
            advance_acc = argv[++i];
            do_nonce = 0;
        } else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) {
            envpath = argv[++i];
        }
    }

    if (load_env(envpath, rpc, sizeof(rpc), priv_b58, sizeof(priv_b58),
                 pub_b58, sizeof(pub_b58)) != 0) {
        fprintf(stderr, "bad env file (need RPC_URL PRIVATE_KEY PUBLIC_KEY)\n");
        return 1;
    }
    if (b58_decode(priv_b58, secret, 64) != 0) {
        memset(secret, 0, sizeof(secret));
        if (b58_decode(priv_b58, secret, 32) != 0) {
            fprintf(stderr, "bad PRIVATE_KEY\n");
            return 1;
        }
    }
    if (b58_decode(pub_b58, want_pk, 32) != 0) {
        fprintf(stderr, "bad PUBLIC_KEY\n");
        return 1;
    }
    if (route0_signer_init(&wallet, secret) != 0) {
        return 1;
    }
    memset(secret, 0, sizeof(secret));
    memset(priv_b58, 0, sizeof(priv_b58));
    if (memcmp(wallet.pk, want_pk, 32) != 0) {
        fprintf(stderr, "signer pubkey mismatch\n");
        return 1;
    }
    if (b58_encode(wallet.pk, 32, want_b58, sizeof(want_b58)) != 0) {
        return 1;
    }
    printf("EXEC-005A  payer %s\n", want_b58);

    if (rpc_balance(rpc, pub_b58, &bal) != 0) {
        fprintf(stderr, "balance\n");
        return 1;
    }
    printf("  balance  %" PRIu64 " lamports\n", bal);
    if (bal < 5000000ull) {
        fprintf(stderr, "balance too low for a safe smoke\n");
        return 1;
    }

    if (advance_acc == NULL) {
        if (rpc_blockhash(rpc, bh) != 0) {
            return 1;
        }
        if (smoke_build_memo(wallet.pk, bh, CU_PRICE, &tx) != 0 ||
            smoke_sign(&wallet, 1, &tx) != 0) {
            fprintf(stderr, "build/sign memo\n");
            return 1;
        }
        printf("  memo tx  %u bytes  (fresh blockhash)\n", tx.len);
        if (send_and_confirm(rpc, &tx, "A memo") != 0) {
            return 1;
        }
    }

    if (advance_acc != NULL) {
        uint8_t nonce_pk[32];
        if (b58_decode(advance_acc, nonce_pk, 32) != 0) {
            fprintf(stderr, "bad --advance\n");
            return 1;
        }
        if (rpc_nonce_hash(rpc, advance_acc, bh) != 0) {
            return 1;
        }
        if (smoke_build_nonce_memo(wallet.pk, nonce_pk, bh, CU_PRICE, &tx) != 0 ||
            smoke_sign(&wallet, 1, &tx) != 0) {
            fprintf(stderr, "build/sign nonce-memo\n");
            return 1;
        }
        if (send_and_confirm(rpc, &tx, "B nonce-memo") != 0) {
            return 1;
        }
        printf("EXEC-005A ok  nonce-memo landed\n");
        return 0;
    }

    if (!do_nonce) {
        printf("EXEC-005A  memo landed  (nonce skipped)\n");
        return 0;
    }

    {
        uint8_t nseed[32];
        route0_signer_t keys[2];
        char nonce_b58[128];

        if (getrandom(nseed, 32, 0) != 32) {
            return 1;
        }
        if (route0_signer_init(&keys[1], nseed) != 0) {
            return 1;
        }
        memset(nseed, 0, sizeof(nseed));
        keys[0] = wallet;
        if (b58_encode(keys[1].pk, 32, nonce_b58, sizeof(nonce_b58)) != 0) {
            return 1;
        }
        if (rpc_rent(rpc, 80, &rent) != 0) {
            fprintf(stderr, "rent\n");
            return 1;
        }
        printf("  nonce   %s  rent %" PRIu64 "\n", nonce_b58, rent);
        if (rpc_blockhash(rpc, bh) != 0) {
            return 1;
        }
        if (smoke_build_create_nonce(wallet.pk, keys[1].pk, bh, rent, CU_PRICE, &tx) != 0 ||
            smoke_sign(keys, 2, &tx) != 0) {
            fprintf(stderr, "build/sign create-nonce\n");
            return 1;
        }
        if (send_and_confirm(rpc, &tx, "B create-nonce") != 0) {
            return 1;
        }
        if (rpc_nonce_hash(rpc, nonce_b58, bh) != 0) {
            return 1;
        }
        if (smoke_build_nonce_memo(wallet.pk, keys[1].pk, bh, CU_PRICE, &tx) != 0 ||
            smoke_sign(&wallet, 1, &tx) != 0) {
            fprintf(stderr, "build/sign nonce-memo\n");
            return 1;
        }
        if (send_and_confirm(rpc, &tx, "B nonce-memo") != 0) {
            return 1;
        }
    }

    printf("EXEC-005A ok  both landed\n");
    return 0;
}
