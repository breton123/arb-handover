/*
 * STATE-005 — follow OrbitFlare FEEDCAP1, emit unique DLMM N to jsonl.
 * No decide. No send. Does not bind 20001. Does not touch feed_live.
 */
#include "classify_n.h"
#include "feed.h"
#include "proto.h"
#include "shred.h"
#include "swapix.h"
#include "tx.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
hex_encode(const uint8_t *p, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2u] = H[p[i] >> 4];
        out[i * 2u + 1u] = H[p[i] & 15u];
    }
    out[n * 2u] = 0;
}

static int
newest_cap(const char *dir, char *out, size_t cap)
{
    DIR *dp;
    struct dirent *de;
    time_t best = 0;
    char name[256];
    int found = 0;

    dp = opendir(dir);
    if (dp == NULL) {
        return -1;
    }
    name[0] = 0;
    while ((de = readdir(dp)) != NULL) {
        size_t L = strlen(de->d_name);
        char path[1024];
        struct stat st;
        if (L < 5 || strcmp(de->d_name + L - 4, ".cap") != 0) {
            continue;
        }
        if (strncmp(de->d_name, "orbitflare-", 11) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (st.st_mtime >= best) {
            best = st.st_mtime;
            snprintf(name, sizeof(name), "%s", de->d_name);
            found = 1;
        }
    }
    closedir(dp);
    if (!found) {
        return -1;
    }
    snprintf(out, cap, "%s/%s", dir, name);
    return 0;
}

static int
follow_slot(FILE *f, feed_slot_t *s)
{
    long pos;
    uint8_t hdr[FEEDCAP_REC_HDR];
    shred_view_t v;
    size_t n;

    pos = ftell(f);
    if (pos < 0) {
        return -1;
    }
    n = fread(hdr, 1, FEEDCAP_REC_HDR, f);
    if (n != FEEDCAP_REC_HDR) {
        clearerr(f);
        if (fseek(f, pos, SEEK_SET) != 0) {
            return -1;
        }
        return 1;
    }
    memcpy(&s->rx_ns, hdr, 8);
    memcpy(&s->rx_tsc, hdr + 8, 8);
    memcpy(&s->len, hdr + 16, 4);
    memcpy(&s->seq, hdr + 20, 4);
    if (s->len < SHRED_COMMON_HDR_SZ || s->len > SHRED_MAX_SZ) {
        if (fseek(f, pos + 1, SEEK_SET) != 0) {
            return -1;
        }
        return 2;
    }
    n = fread(s->data, 1, s->len, f);
    if (n != s->len) {
        clearerr(f);
        if (fseek(f, pos, SEEK_SET) != 0) {
            return -1;
        }
        return 1;
    }
    if (shred_parse(s->data, (uint16_t)s->len, &v) != 0) {
        if (fseek(f, pos + 1, SEEK_SET) != 0) {
            return -1;
        }
        return 2;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *out_path = NULL;
    FILE *cf = NULL;
    FILE *out;
    feedcap_hdr_t hdr;
    char cap_path[1024];
    hot_seen_t seen;
    int start_at_eof = 1;
    uint64_t n_emit = 0;
    uint64_t n_shred = 0;
    int ai;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--dir") == 0 && ai + 1 < argc) {
            dir = argv[++ai];
        } else if (strcmp(argv[ai], "--out") == 0 && ai + 1 < argc) {
            out_path = argv[++ai];
        } else {
            fprintf(stderr, "usage: %s --dir CAPDIR --out seen_n.jsonl\n", argv[0]);
            return 1;
        }
    }
    if (dir == NULL || out_path == NULL) {
        fprintf(stderr, "need --dir --out\n");
        return 1;
    }
    out = fopen(out_path, "a");
    if (out == NULL) {
        perror(out_path);
        return 1;
    }
    setvbuf(out, NULL, _IOLBF, 0);
    if (hot_seen_init(&seen) != 0) {
        return 1;
    }
    fprintf(stderr, "STATE-005-N  follow %s  out %s  NO SEND\n", dir, out_path);
    cap_path[0] = 0;
    for (;;) {
        feed_slot_t slot;
        int rc;

        if (cf == NULL) {
            if (newest_cap(dir, cap_path, sizeof(cap_path)) != 0) {
                usleep(50000);
                continue;
            }
            cf = fopen(cap_path, "rb");
            if (cf == NULL) {
                usleep(50000);
                continue;
            }
            if (feedcap_read_header(cf, &hdr) != 0) {
                fclose(cf);
                cf = NULL;
                usleep(50000);
                continue;
            }
            if (start_at_eof) {
                if (fseek(cf, 0, SEEK_END) != 0) {
                    fclose(cf);
                    cf = NULL;
                    continue;
                }
                fprintf(stderr, "follow %s from EOF\n", cap_path);
                start_at_eof = 0;
            } else {
                fprintf(stderr, "follow %s from header (rotate)\n", cap_path);
            }
        }
        rc = follow_slot(cf, &slot);
        if (rc == 2) {
            continue;
        }
        if (rc == 1) {
            char newer[1024];
            usleep(2000);
            if (newest_cap(dir, newer, sizeof(newer)) == 0
                && strcmp(newer, cap_path) != 0) {
                fclose(cf);
                cf = NULL;
                snprintf(cap_path, sizeof(cap_path), "%s", newer);
            }
            continue;
        }
        if (rc != 0) {
            fclose(cf);
            cf = NULL;
            continue;
        }
        {
            shred_view_t v;
            uint16_t plen;
            swapix_t nix;
            char sig_hex[129];
            char pool_hex[65];

            if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
                continue;
            }
            n_shred++;
            plen = (uint16_t)(slot.len - (uint16_t)(v.payload - slot.data));
            if (classify_n(v.payload, plen, 6) == REL_N_NONE) {
                continue;
            }
            if (swapix_from_payload(v.payload, plen, &nix) != 0 || !nix.have_sig) {
                continue;
            }
            if (nix.protocol != PROTO_DLMM) {
                continue;
            }
            if (hot_seen_first(&seen, nix.sig) != 1) {
                continue;
            }
            hex_encode(nix.sig, 64, sig_hex);
            hex_encode(nix.pool, 32, pool_hex);
            fprintf(out,
                    "{\"sig\":\"%s\",\"pool\":\"%s\",\"n_ain\":%" PRIu64
                    ",\"n_dir\":%u,\"min_out\":%" PRIu64
                    ",\"slot\":%" PRIu64 ",\"rx_ns\":%" PRIu64 "}\n",
                    sig_hex, pool_hex, nix.amount_in, (unsigned)nix.direction,
                    nix.min_out, v.slot, slot.rx_ns);
            n_emit++;
            if (n_emit <= 3 || (n_emit % 20ull) == 0) {
                fprintf(stderr, "STATE-005-N  emit=%" PRIu64 " shred=%" PRIu64 "\n",
                        n_emit, n_shred);
            }
        }
    }
}
