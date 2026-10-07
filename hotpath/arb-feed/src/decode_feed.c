/*
 * Decode unique supported N from FEEDCAP1 shreds.
 * One signature → one print. Fragments of the same tx are dups.
 */
#include "feed.h"
#include "shred.h"
#include "swapix.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
walk(const char *path, hot_seen_t *seen, uint32_t limit,
     uint64_t *shreds, uint64_t *hits, uint64_t *dups)
{
    FILE *f;
    feedcap_hdr_t hdr;
    feed_slot_t slot;

    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (feedcap_read_header(f, &hdr) != 0) {
        fclose(f);
        return -1;
    }
    (void)hdr;
    fprintf(stderr, "read  %s\n", path);
    for (;;) {
        shred_view_t v;
        uint16_t plen;
        swapix_t ix;
        int rc = feedcap_read_slot(f, &slot);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            fclose(f);
            return -1;
        }
        if (shred_parse(slot.data, (uint16_t)slot.len, &v) != 0) {
            continue;
        }
        (*shreds)++;
        plen = (uint16_t)(slot.len - (uint16_t)(v.payload - slot.data));
        if (swapix_from_payload(v.payload, plen, &ix) != 0) {
            continue;
        }
        (*hits)++;
        if (hot_seen_first(seen, ix.sig) != 1) {
            (*dups)++;
            continue;
        }
        {
            uint32_t b;
            printf("{\"slot\":%" PRIu64 ",\"proto\":%u,\"variant\":%u,\"dir\":%u,"
                   "\"amount_in\":%" PRIu64 ",\"min_out\":%" PRIu64 ",\"pool\":\"",
                   v.slot, ix.protocol, ix.variant, ix.direction,
                   ix.amount_in, ix.min_out);
            for (b = 0; b < 32; b++) {
                printf("%02x", ix.pool[b]);
            }
            printf("\",\"sig\":\"");
            for (b = 0; b < 64; b++) {
                printf("%02x", ix.sig[b]);
            }
            printf("\"}\n");
        }
    }
    fclose(f);
    if (limit && seen->n >= limit) {
        return 1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    const char *dir = NULL;
    uint32_t limit = 80;
    hot_seen_t seen;
    uint64_t shreds = 0;
    uint64_t hits = 0;
    uint64_t dups = 0;
    DIR *dp;
    struct dirent *de;
    int ai;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--dir") == 0 && ai + 1 < argc) {
            dir = argv[++ai];
        } else if (strcmp(argv[ai], "--limit") == 0 && ai + 1 < argc) {
            limit = (uint32_t)strtoul(argv[++ai], NULL, 10);
        }
    }
    if (dir == NULL) {
        fprintf(stderr, "usage: %s --dir DIR [--limit N]\n", argv[0]);
        return 1;
    }
    if (hot_seen_init(&seen) != 0) {
        return 1;
    }
    dp = opendir(dir);
    if (dp == NULL) {
        return 1;
    }
    while ((de = readdir(dp)) != NULL) {
        size_t L = strlen(de->d_name);
        char path[1024];
        int rc;
        if (L < 5 || strcmp(de->d_name + L - 4, ".cap") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        rc = walk(path, &seen, limit, &shreds, &hits, &dups);
        if (rc == 1) {
            break;
        }
    }
    closedir(dp);
    fprintf(stderr, "DECODE-FEED  shreds=%" PRIu64 " decoded=%" PRIu64
                    " unique=%u dups=%" PRIu64 "\n",
            shreds, hits, seen.n, dups);
    hot_seen_free(&seen);
    return 0;
}
