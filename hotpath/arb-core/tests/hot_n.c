#include "hot.h"
#include "swapix.h"
#include "syncrec.h"
#include "classify.h"
#include "dlmm.h"
#include "pump.h"

#include <dirent.h>
#include <inttypes.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { \
    if (!(c)) { fprintf(stderr, "FAIL  %s\n", (m)); return 1; } \
} while (0)

static const uint8_t PUMP_SELL[8] = {
    0x33, 0xe6, 0x85, 0xa4, 0x01, 0x7f, 0x83, 0xad
};
static const uint8_t PUMP_BUY[8] = {
    0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb, 0xea
};
static const uint8_t PUMP_BUY_EQ[8] = {
    0xc6, 0x2e, 0x15, 0x52, 0xb4, 0xd9, 0xe8, 0x70
};
static const uint8_t PROG_TOKEN[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93,
    0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91,
    0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9
};
static const uint8_t PROG_ATA[32] = {
    0x8c, 0x97, 0x25, 0x8f, 0x4e, 0x24, 0x89, 0xf1,
    0xbb, 0x3d, 0x10, 0x29, 0x14, 0x8e, 0x0d, 0x83,
    0x0b, 0x5a, 0x13, 0x99, 0xda, 0xff, 0x10, 0x84,
    0x04, 0x8e, 0x7b, 0xd8, 0xdb, 0xe9, 0xf8, 0x59
};

static void
put_u64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static int
on_curve(const uint8_t pt[32])
{
    uint8_t yb[32];
    BN_CTX *ctx;
    BIGNUM *y, *p, *d, *y2, *u, *v, *x2, *exp, *res, *pm1;
    int on = 0;

    memcpy(yb, pt, 32);
    yb[31] &= 0x7f;
    ctx = BN_CTX_new();
    if (ctx == NULL) {
        return 0;
    }
    BN_CTX_start(ctx);
    y = BN_CTX_get(ctx);
    p = BN_CTX_get(ctx);
    d = BN_CTX_get(ctx);
    y2 = BN_CTX_get(ctx);
    u = BN_CTX_get(ctx);
    v = BN_CTX_get(ctx);
    x2 = BN_CTX_get(ctx);
    exp = BN_CTX_get(ctx);
    res = BN_CTX_get(ctx);
    pm1 = BN_CTX_get(ctx);
    {
        BIGNUM *pp = p;
        BIGNUM *dd = d;
        if (pm1 == NULL
            || BN_lebin2bn(yb, 32, y) == NULL
            || BN_hex2bn(&pp, "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed") <= 0
            || BN_hex2bn(&dd, "52036cee2b6ffe738cc740797779e89800700a4d4141d8ab75eb4dca135978a3") <= 0) {
            BN_CTX_end(ctx);
            BN_CTX_free(ctx);
            return 0;
        }
    }
    if (BN_cmp(y, p) >= 0) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
        return 0;
    }
    BN_mod_mul(y2, y, y, p, ctx);
    BN_copy(u, y2);
    BN_sub_word(u, 1);
    if (BN_is_negative(u)) {
        BN_add(u, u, p);
    }
    BN_mod_mul(v, d, y2, p, ctx);
    BN_add_word(v, 1);
    BN_mod(v, v, p, ctx);
    if (BN_mod_inverse(x2, v, p, ctx) == NULL) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
        return 0;
    }
    BN_mod_mul(x2, u, x2, p, ctx);
    BN_copy(pm1, p);
    BN_sub_word(pm1, 1);
    BN_rshift1(exp, pm1);
    BN_mod_exp(res, x2, exp, p, ctx);
    on = BN_cmp(res, pm1) != 0;
    BN_CTX_end(ctx);
    BN_CTX_free(ctx);
    return on;
}

static int
ata_addr(const uint8_t owner[32], const uint8_t mint[32], uint8_t out[32])
{
    uint8_t bump;
    uint8_t digest[32];

    for (bump = 255; ; bump--) {
        EVP_MD_CTX *md = EVP_MD_CTX_new();
        if (md == NULL
            || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1
            || EVP_DigestUpdate(md, owner, 32) != 1
            || EVP_DigestUpdate(md, PROG_TOKEN, 32) != 1
            || EVP_DigestUpdate(md, mint, 32) != 1
            || EVP_DigestUpdate(md, &bump, 1) != 1
            || EVP_DigestUpdate(md, PROG_ATA, 32) != 1
            || EVP_DigestUpdate(md, "ProgramDerivedAddress", 21) != 1
            || EVP_DigestFinal_ex(md, digest, NULL) != 1) {
            EVP_MD_CTX_free(md);
            return -1;
        }
        EVP_MD_CTX_free(md);
        if (!on_curve(digest)) {
            memcpy(out, digest, 32);
            return 0;
        }
        if (bump == 0) {
            return -1;
        }
    }
}

static uint16_t
build_pump_tx(uint8_t *buf, uint16_t cap, const uint8_t disc[8],
              uint64_t ain, uint64_t mino, const uint8_t sig[64],
              const uint8_t pool[32])
{
    uint16_t o = 0;
    if (cap < 300) {
        return 0;
    }
    buf[o++] = 1;
    memcpy(buf + o, sig, 64);
    o += 64;
    buf[o++] = 1;
    buf[o++] = 0;
    buf[o++] = 1;
    buf[o++] = 3;
    memset(buf + o, 0x11, 32);
    o += 32;
    memcpy(buf + o, PROG_PUMP, 32);
    o += 32;
    memcpy(buf + o, pool, 32);
    o += 32;
    memset(buf + o, 0xBB, 32);
    o += 32;
    buf[o++] = 1;
    buf[o++] = 1;
    buf[o++] = 1;
    buf[o++] = 2;
    buf[o++] = 24;
    memcpy(buf + o, disc, 8);
    o += 8;
    put_u64(buf + o, ain);
    o += 8;
    put_u64(buf + o, mino);
    o += 8;
    return o;
}

static uint16_t
build_dlmm_tx(uint8_t *buf, uint16_t cap, uint64_t ain, uint64_t mino,
              const uint8_t sig[64], const uint8_t pool[32],
              uint8_t swap_for_y)
{
    uint8_t user[32];
    uint8_t mint_x[32];
    uint8_t mint_y[32];
    uint8_t ata_in[32];
    uint8_t ata_out[32];
    uint16_t o = 0;

    memset(user, 0xA1, 32);
    user[31] = 1;
    memset(mint_x, 0xB1, 32);
    mint_x[31] = 2;
    memset(mint_y, 0xC1, 32);
    mint_y[31] = 3;
    if (ata_addr(user, swap_for_y ? mint_x : mint_y, ata_in) != 0
        || ata_addr(user, swap_for_y ? mint_y : mint_x, ata_out) != 0) {
        return 0;
    }
    if (cap < 700) {
        return 0;
    }
    buf[o++] = 1;
    memcpy(buf + o, sig, 64);
    o += 64;
    buf[o++] = 1;
    buf[o++] = 0;
    buf[o++] = 13;
    buf[o++] = 14;
    memcpy(buf + o, user, 32);
    o += 32;
    memcpy(buf + o, PROG_DLMM, 32);
    o += 32;
    memcpy(buf + o, pool, 32);
    o += 32;
    memset(buf + o, 0x21, 32);
    o += 32; /* bitmap */
    memset(buf + o, 0x22, 32);
    o += 32; /* rx */
    memset(buf + o, 0x23, 32);
    o += 32; /* ry */
    memcpy(buf + o, ata_in, 32);
    o += 32;
    memcpy(buf + o, ata_out, 32);
    o += 32;
    memcpy(buf + o, mint_x, 32);
    o += 32;
    memcpy(buf + o, mint_y, 32);
    o += 32;
    memset(buf + o, 0x28, 32);
    o += 32; /* oracle */
    memset(buf + o, 0x29, 32);
    o += 32; /* host */
    memcpy(buf + o, PROG_TOKEN, 32);
    o += 32; /* token x prog @11 */
    memcpy(buf + o, PROG_TOKEN, 32);
    o += 32; /* token y prog @12 */
    memset(buf + o, 0xBB, 32);
    o += 32;
    buf[o++] = 1;
    buf[o++] = 1; /* prog = DLMM */
    buf[o++] = 13;
    {
        /* keys: 0 user, 1 PROG_DLMM, 2 pool, 3 bitmap, 4 rx, 5 ry,
                 6 ata_in, 7 ata_out, 8 mint_x, 9 mint_y, 10 oracle,
                 11 host, 12 token_x, 13 token_y */
        const uint8_t acc[13] = {
            2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 12, 13
        };
        memcpy(buf + o, acc, 13);
        o += 13;
    }
    buf[o++] = 24;
    memcpy(buf + o, DLMM_SWAP2_DISC, 8);
    o += 8;
    put_u64(buf + o, ain);
    o += 8;
    put_u64(buf + o, mino);
    o += 8;
    return o;
}

static void
print_ix(const swapix_t *ix, int ok)
{
    uint32_t b;
    printf("{\"ok\":%d", ok);
    if (ok == 0 || ix == NULL) {
        printf("}\n");
        return;
    }
    printf(",\"proto\":%u,\"variant\":%u,\"dir\":%u,\"amount_in\":%" PRIu64
           ",\"min_out\":%" PRIu64 ",\"pool\":\"",
           ix->protocol, ix->variant, ix->direction, ix->amount_in, ix->min_out);
    for (b = 0; b < 32; b++) {
        printf("%02x", ix->pool[b]);
    }
    printf("\",\"sig\":\"");
    for (b = 0; b < 64; b++) {
        printf("%02x", ix->sig[b]);
    }
    printf("\"}\n");
}

static int
selftest(void)
{
    uint8_t tx[800];
    uint8_t frag[900];
    uint8_t sig[64];
    uint8_t pool[32];
    uint16_t n;
    swapix_t ix;
    hot_trigger_t trig;
    hot_seen_t seen;
    uint32_t decides = 0;
    uint32_t i;

    memset(sig, 0x5A, 64);
    memset(pool, 0xE0, 32);
    pool[0] = 0x42;

    n = build_pump_tx(tx, sizeof(tx), PUMP_SELL, 123456789ull, 1ull, sig, pool);
    CHECK(n > 0, "build sell");
    CHECK(swapix_from_payload(tx, n, &ix) == 0, "decode sell");
    CHECK(ix.protocol == PROTO_PUMP, "sell proto");
    CHECK(ix.amount_in == 123456789ull && ix.min_out == 1ull, "sell amounts");
    CHECK(ix.direction == PUMP_DIR_BASE_TO_QUOTE, "sell dir");
    CHECK(memcmp(ix.pool, pool, 32) == 0, "sell pool");
    CHECK(ix.have_sig && memcmp(ix.sig, sig, 64) == 0, "sell sig");
    CHECK(swapix_to_trigger(&ix, &trig) == 0
          && trig.pump.amount_in == 123456789ull, "to_trigger");

    n = build_pump_tx(tx, sizeof(tx), PUMP_BUY_EQ, 999ull, 10ull, sig, pool);
    CHECK(n > 0 && swapix_from_payload(tx, n, &ix) == 0, "decode buy_eq");
    CHECK(ix.direction == PUMP_DIR_QUOTE_TO_BASE && ix.amount_in == 999ull,
          "buy_eq fields");

    n = build_pump_tx(tx, sizeof(tx), PUMP_BUY, 50ull, 100ull, sig, pool);
    CHECK(n > 0 && swapix_from_payload(tx, n, &ix) != 0, "buy exact-out fail-closed");

    n = build_dlmm_tx(tx, sizeof(tx), 777ull, 3ull, sig, pool, 1);
    CHECK(n > 0, "build dlmm");
    CHECK(swapix_from_payload(tx, n, &ix) == 0, "decode dlmm");
    CHECK(ix.protocol == PROTO_DLMM && ix.amount_in == 777ull, "dlmm ain");
    CHECK(ix.direction == 1, "dlmm swap_for_y");

    n = build_dlmm_tx(tx, sizeof(tx), 888ull, 4ull, sig, pool, 0);
    CHECK(n > 0 && swapix_from_payload(tx, n, &ix) == 0, "decode dlmm y->x");
    CHECK(ix.direction == 0, "dlmm swap_for_y 0");

    /* One tx across fragments → exactly one decide. */
    n = build_pump_tx(tx, sizeof(tx), PUMP_SELL, 42ull, 0ull, sig, pool);
    CHECK(n > 0 && hot_seen_init(&seen) == 0, "seen init");
    CHECK(swapix_from_payload(tx, 40, &ix) != 0, "prefix too short");
    memset(frag, 0xCC, 16);
    memcpy(frag + 16, tx, n);
    for (i = 0; i < 5; i++) {
        const uint8_t *p = tx;
        uint16_t ln = n;
        if (i == 0) {
            p = tx;
            ln = (uint16_t)(n / 2);
        } else if (i == 1) {
            p = frag;
            ln = (uint16_t)(16u + n);
        } else {
            p = tx;
            ln = n;
        }
        if (swapix_from_payload(p, ln, &ix) == 0
            && swapix_to_trigger(&ix, &trig) == 0
            && hot_seen_first(&seen, ix.sig) == 1) {
            decides++;
        }
    }
    CHECK(decides == 1, "one tx → one decide");
    hot_seen_free(&seen);

    {
        syncrec_t rec;
        CHECK(syncrec_open(&rec, "/tmp/hot_n_sync.bin", 4194000000ull) == 0,
              "sync open");
        CHECK(syncrec_exec(&rec, 1, sig, 10, 20, SYN_EXEC_NONE) == 0, "sync exec");
        syncrec_close(&rec);
    }
    printf("HOT-N  selftest ok  sell/buy_eq/dlmm ATA  buy fail-closed  "
           "1 tx → 1 decide\n");
    return 0;
}

static int
decode_file(const char *path)
{
    FILE *f;
    uint8_t buf[4096];
    size_t n;
    swapix_t ix;
    int rc;

    f = fopen(path, "rb");
    if (f == NULL) {
        print_ix(NULL, 0);
        return 1;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0 || n > 0xffffu) {
        print_ix(NULL, 0);
        return 1;
    }
    rc = swapix_from_payload(buf, (uint16_t)n, &ix);
    print_ix(&ix, rc == 0);
    return rc == 0 ? 0 : 2;
}

static int
decode_corpus(const char *dir)
{
    DIR *dp;
    struct dirent *de;
    int fail = 0;
    uint32_t n = 0;
    uint32_t ok = 0;

    dp = opendir(dir);
    if (dp == NULL) {
        return 1;
    }
    while ((de = readdir(dp)) != NULL) {
        size_t L = strlen(de->d_name);
        char path[1024];
        if (L < 5 || strcmp(de->d_name + L - 4, ".bin") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        printf("{\"file\":\"%s\",", de->d_name);
        {
            FILE *f = fopen(path, "rb");
            uint8_t buf[4096];
            size_t rn;
            swapix_t ix;
            int rc;
            n++;
            if (f == NULL) {
                printf("\"ok\":0}\n");
                fail++;
                continue;
            }
            rn = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            rc = (rn > 0 && rn <= 0xffffu)
                ? swapix_from_payload(buf, (uint16_t)rn, &ix)
                : -1;
            if (rc == 0) {
                ok++;
            }
            /* reprint without leading brace */
            {
                uint32_t b;
                printf("\"ok\":%d", rc == 0);
                if (rc == 0) {
                    printf(",\"proto\":%u,\"variant\":%u,\"dir\":%u,"
                           "\"amount_in\":%" PRIu64 ",\"min_out\":%" PRIu64
                           ",\"pool\":\"",
                           ix.protocol, ix.variant, ix.direction,
                           ix.amount_in, ix.min_out);
                    for (b = 0; b < 32; b++) {
                        printf("%02x", ix.pool[b]);
                    }
                    printf("\",\"sig\":\"");
                    for (b = 0; b < 64; b++) {
                        printf("%02x", ix.sig[b]);
                    }
                    printf("\"");
                }
                printf("}\n");
            }
        }
    }
    closedir(dp);
    fprintf(stderr, "HOT-N  corpus  ok=%u n=%u fail=%d\n", ok, n, fail);
    return fail ? 1 : 0;
}

int
main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        return selftest();
    }
    if (argc >= 3 && strcmp(argv[1], "--decode") == 0) {
        return decode_file(argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "--corpus") == 0) {
        return decode_corpus(argv[2]);
    }
    fprintf(stderr, "usage: %s --selftest | --decode FILE | --corpus DIR\n",
            argv[0]);
    return 1;
}
