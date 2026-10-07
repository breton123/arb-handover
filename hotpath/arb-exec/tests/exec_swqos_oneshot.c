#include "swqos.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int
load_kv(const char *path)
{
    FILE *f;
    char line[512];
    int n = 0;

    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq, *nl, *k, *v;
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
        if (strncmp(line, "export ", 7) == 0) {
            k = line + 7;
        } else {
            k = line;
        }
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
            n++;
        }
        memset(eq + 1, 0, strlen(v));
    }
    fclose(f);
    return n;
}

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int
main(int argc, char **argv)
{
    FILE *f;
    uint8_t tx[SWQOS_TX_MAX];
    size_t nread;
    uint16_t len;
    int rc;
    int i;
    uint64_t t0;
    uint64_t t1;

    if (argc != 2) {
        fprintf(stderr, "usage: exec_swqos_oneshot <signed.tx>\n");
        return 2;
    }
    load_kv("/home/louis/.arb-smoke.env");
    load_kv("/home/louis/.arb-swqos.env");
    load_kv("/home/louis/arb-exec/.deploy/swqos.env");

    if (getenv("SWQOS_KEY") == NULL && getenv("SWQOS_API_KEY") == NULL) {
        fprintf(stderr, "set SWQOS_KEY\n");
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    nread = fread(tx, 1, sizeof(tx), f);
    fclose(f);
    if (nread < 64 || nread > SWQOS_TX_MAX) {
        fprintf(stderr, "bad tx len %zu\n", nread);
        return 1;
    }
    len = (uint16_t)nread;

    rc = swqos_open_env();
    if (rc != 0) {
        fprintf(stderr, "swqos_open %d\n", rc);
        return 1;
    }
    for (i = 0; i < 80 && swqos_ready_n() < 1; i++) {
        struct timespec sl = {0, 50000000};
        nanosleep(&sl, NULL);
    }
    if (swqos_ready_n() < 1) {
        fprintf(stderr, "swqos not ready\n");
        swqos_close();
        return 1;
    }
    t0 = now_ns();
    rc = swqos_send(tx, len);
    t1 = now_ns();
    printf("ONESHOT_SWQOS rc=%d send_ns=%" PRIu64 " ready=%d conn=%d len=%u\n",
           rc, t1 - t0, swqos_ready_n(), swqos_last_conn(), (unsigned)len);
    swqos_close();
    return rc == 0 ? 0 : 1;
}
