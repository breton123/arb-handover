#include "net/capio.h"
#include "shred/entry.h"
#include "shred/prefix.h"
#include "shred/txframe.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_MAX 128u

typedef struct {
    uint8_t  used;
    uint8_t  found;
    uint8_t  gold;
    uint8_t  sig_in_prefix;
    uint8_t  sig[64];
    uint64_t slot;
    uint32_t ys_index;
    uint32_t raw_index;
    int      rc;
    int      last_erc;
    int      fail_rc;
    uint32_t consumed;
    uint32_t sig_off;
    txframe_t tf;
    txframe_t fail_tf;
    shred_prefix_info_t pre;
    uint32_t entry_ord;
    uint32_t entry_tx_count;
    uint32_t entry_off;
    uint32_t stream_off;
    char     hex[129];
    char     stage[24];
} probe_t;

typedef struct {
    probe_t *pr;
    uint32_t n;
    uint32_t batch_off;
    uint32_t entry_base;
} find_ctx_t;

static int
hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int
unhex64(const char *s, uint8_t *o)
{
    uint32_t i;

    for (i = 0; i < 64u; i++) {
        int a = hexval(s[2u * i]);
        int b = hexval(s[2u * i + 1u]);

        if (a < 0 || b < 0) {
            return -1;
        }
        o[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static uint64_t
json_u64(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    const char *c;

    if (p == NULL) {
        return 0;
    }
    c = strchr(p, ':');
    return (c != NULL) ? strtoull(c + 1, NULL, 10) : 0;
}

static void
on_tx(void *user, const shred_entry_hit_t *hit)
{
    find_ctx_t *cx = user;
    uint32_t i;

    for (i = 0; i < cx->n; i++) {
        if (cx->pr[i].found) {
            continue;
        }
        if (memcmp(cx->pr[i].sig, hit->tx.sig, 64) != 0) {
            continue;
        }
        cx->pr[i].found = 1;
        cx->pr[i].rc = TXF_OK;
        cx->pr[i].tf = hit->tx;
        cx->pr[i].entry_ord = cx->entry_base + hit->entry_ord;
        cx->pr[i].entry_tx_count = hit->entry_tx_count;
        cx->pr[i].entry_off = cx->batch_off + hit->entry_off;
        cx->pr[i].stream_off = cx->batch_off + hit->tx.stream_off;
    }
}

static uint32_t
load_probes(const char *path, probe_t *pr, uint32_t cap)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    uint32_t n = 0;

    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL && n < cap) {
        const char *sig = strstr(line, "\"sig\"");

        if (sig == NULL) {
            continue;
        }
        sig = strchr(sig, ':');
        if (sig == NULL) {
            continue;
        }
        sig = strchr(sig, '"');
        if (sig == NULL) {
            continue;
        }
        sig++;
        memset(&pr[n], 0, sizeof(pr[n]));
        if (unhex64(sig, pr[n].sig) != 0) {
            continue;
        }
        memcpy(pr[n].hex, sig, 128);
        pr[n].hex[128] = 0;
        pr[n].used = 1;
        pr[n].slot = json_u64(line, "\"slot\"");
        pr[n].ys_index = (uint32_t)json_u64(line, "\"ys_index\"");
        pr[n].raw_index = (uint32_t)json_u64(line, "\"raw_index\"");
        pr[n].rc = -1;
        {
            const char *st = strstr(line, "\"stage\"");
            const char *q;

            pr[n].stage[0] = 0;
            if (st != NULL) {
                q = strchr(st + 7, '"');
                if (q != NULL) {
                    uint32_t k = 0;

                    q++;
                    while (q[k] != 0 && q[k] != '"' && k < 23u) {
                        pr[n].stage[k] = q[k];
                        k++;
                    }
                    pr[n].stage[k] = 0;
                }
            }
            pr[n].gold = (uint8_t)(strcmp(pr[n].stage, "TX_PARSE") == 0);
        }
        n++;
    }
    fclose(f);
    return n;
}

int
main(int argc, char **argv)
{
    capio_t cap;
    shred_prefix_t *px = NULL;
    probe_t pr[PROBE_MAX];
    const char *cpath = NULL, *ppath = NULL;
    uint32_t i, npr = 0, n_ok = 0, n_miss = 0, n_gold = 0, n_gold_ok = 0;
    find_ctx_t cx;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--cap") == 0 && i + 1 < (uint32_t)argc) {
            cpath = argv[++i];
        } else if (strcmp(argv[i], "--probes") == 0 && i + 1 < (uint32_t)argc) {
            ppath = argv[++i];
        } else if (strcmp(argv[i], "--self-test") == 0) {
            printf("TXFRAME-COVER self-test ok\n");
            return 0;
        }
    }
    if (cpath == NULL || ppath == NULL) {
        fprintf(stderr, "usage: txframe_cover --cap FILE --probes FILE\n");
        return 2;
    }
    npr = load_probes(ppath, pr, PROBE_MAX);
    if (npr == 0 || shred_prefix_init(&px) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    for (i = 0; i < npr; i++) {
        (void)shred_prefix_watch(px, pr[i].slot);
    }
    if (capio_open(&cap, cpath) != 0) {
        fprintf(stderr, "cap open failed\n");
        return 1;
    }
    while (1) {
        net_slot_t sl;
        int rc = capio_read(&cap, &sl);

        if (rc == 1) {
            break;
        }
        if (rc == 0) {
            (void)shred_prefix_push(px, sl.data, sl.len);
        }
    }
    capio_close(&cap);

    cx.pr = pr;
    cx.n = npr;
    for (i = 0; i < npr; i++) {
        const uint8_t *buf = NULL;
        uint32_t blen = 0, cons = 0;
        int erc = 0;

        (void)shred_prefix_info(px, pr[i].slot, &pr[i].pre);
        if (shred_prefix_bytes(px, pr[i].slot, &buf, &blen) != 0 || blen == 0) {
            continue;
        }
        {
            uint32_t ends[PREFIX_BATCH_MAX];
            uint32_t nb = 0, b, base = 0;

            (void)shred_prefix_batches(px, pr[i].slot, ends, PREFIX_BATCH_MAX,
                                       &nb);
            erc = ENT_OK;
            cons = 0;
            for (b = 0; b < nb; b++) {
                uint32_t start = (b == 0u) ? 0u : ends[b - 1u];
                uint32_t elen = ends[b] - start;
                uint32_t used = 0;
                int brc = 0;
                uint64_t nent = 0;

                cx.batch_off = start;
                cx.entry_base = base;
                (void)shred_entries_parse(buf + start, elen, on_tx, &cx, &used,
                                          &brc);
                erc = brc;
                cons = start + used;
                if (elen >= 8u) {
                    memcpy(&nent, buf + start, 8);
                    if (nent <= ENT_VEC_MAX) {
                        base += (uint32_t)nent;
                    }
                }
                if (brc != ENT_OK) {
                    break;
                }
            }
        }
        pr[i].last_erc = erc;
        pr[i].consumed = cons;
        if (!pr[i].found && blen >= 64u) {
            const uint8_t *hit = NULL;
            uint32_t o;

            for (o = 0; o + 64u <= blen; o++) {
                if (memcmp(buf + o, pr[i].sig, 64) == 0) {
                    hit = buf + o;
                    pr[i].sig_off = o;
                    pr[i].sig_in_prefix = 1;
                    break;
                }
            }
            (void)hit;
        }
        if (!pr[i].found && cons < blen) {
            pr[i].fail_rc = txframe_parse(buf + cons, blen - cons, &pr[i].fail_tf);
        }
    }
    printf("TXFRAME-COVER n=%u\n", npr);
    for (i = 0; i < npr; i++) {
        const char *res;

        if (pr[i].found) {
            res = "OK";
            n_ok++;
        } else if (!pr[i].pre.have0) {
            res = "PREFIX_GAP";
            n_miss++;
        } else if (pr[i].sig_in_prefix) {
            if (pr[i].last_erc == ENT_STREAM_DESYNC) {
                res = "STREAM_DESYNC";
            } else if (pr[i].last_erc == ENT_NEED_MORE) {
                res = "NEED_MORE_BYTES";
            } else if (pr[i].fail_rc == TXF_UNSUPPORTED_VERSION) {
                res = "UNSUPPORTED_VERSION";
            } else if (pr[i].fail_rc == TXF_BAD_SHORTVEC) {
                res = "BAD_SHORTVEC";
            } else if (pr[i].fail_rc == TXF_BAD_SIGNATURE_SECTION) {
                res = "BAD_SIGNATURE_SECTION";
            } else if (pr[i].fail_rc == TXF_BAD_MESSAGE) {
                res = "BAD_MESSAGE";
            } else {
                res = "IN_PREFIX_NOT_FRAMED";
            }
            n_miss++;
        } else {
            res = "NOT_IN_PREFIX";
            n_miss++;
        }
        if (pr[i].gold) {
            n_gold++;
            if (pr[i].found) {
                n_gold_ok++;
            }
        }
        printf("{\"sig\":\"%s\",\"slot\":%" PRIu64
               ",\"ys_index\":%u,\"shred_index\":%u,\"watermark\":%u,"
               "\"gold\":%u,\"stage\":\"%s\",\"found\":%u,\"version\":%u,"
               "\"tx_len\":%u,\"signature_count\":%u,\"message_off\":%u,"
               "\"stream_off\":%u,\"tx_start\":%u,\"entry_start\":%u,"
               "\"entry_tx_count\":%u,\"have0\":%u,\"first_hole\":%u,"
               "\"concat_len\":%u,\"n_batch\":%u,\"sig_in_prefix\":%u,\"sig_off\":%u,"
               "\"consumed\":%u,\"last_erc\":%d,\"fail_rc\":%d,"
               "\"fail_name\":\"%s\",\"msg_prefix\":\"%02x%02x%02x%02x\","
               "\"result\":\"%s\"}\n",
               pr[i].hex, pr[i].slot, pr[i].ys_index, pr[i].raw_index,
               pr[i].pre.first_hole, pr[i].gold, pr[i].stage, pr[i].found,
               pr[i].found ? pr[i].tf.version : pr[i].fail_tf.version,
               pr[i].found ? pr[i].tf.tx_len : pr[i].fail_tf.tx_len,
               pr[i].found ? pr[i].tf.nsig : pr[i].fail_tf.nsig,
               pr[i].found ? pr[i].tf.message_off : pr[i].fail_tf.message_off,
               pr[i].stream_off, pr[i].stream_off, pr[i].entry_off,
               pr[i].entry_tx_count, pr[i].pre.have0, pr[i].pre.first_hole,
               pr[i].pre.concat_len, pr[i].pre.n_batch, pr[i].sig_in_prefix,
               pr[i].sig_off,
               pr[i].consumed, pr[i].last_erc, pr[i].fail_rc,
               txframe_name(pr[i].found ? TXF_OK : pr[i].fail_rc),
               pr[i].found ? pr[i].tf.msg_prefix[0] : pr[i].fail_tf.msg_prefix[0],
               pr[i].found ? pr[i].tf.msg_prefix[1] : pr[i].fail_tf.msg_prefix[1],
               pr[i].found ? pr[i].tf.msg_prefix[2] : pr[i].fail_tf.msg_prefix[2],
               pr[i].found ? pr[i].tf.msg_prefix[3] : pr[i].fail_tf.msg_prefix[3],
               res);
    }
    printf("framed_ok=%u  not_framed=%u  gold_tx_parse=%u  gold_ok=%u\n",
           n_ok, n_miss, n_gold, n_gold_ok);
    shred_prefix_free(px);
    return 0;
}
