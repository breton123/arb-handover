/*
 * Resident SWQOS send. Open the READY pool once. Hot path is
 * read-tx / swqos_send / reply. No DNS, handshake, or reconnect
 * on the opportunity. Does not replace leader_send().
 */
#include "swqos.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCK_PATH "/home/louis/arb-cap/oneshot/swqos.sock"

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
        k = (strncmp(line, "export ", 7) == 0) ? line + 7 : line;
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
main(void)
{
    int ls, i;
    struct sockaddr_un addr;

    load_kv("/home/louis/.arb-smoke.env");
    load_kv("/home/louis/.arb-swqos.env");
    load_kv("/home/louis/arb-exec/.deploy/swqos.env");
    if (getenv("SWQOS_KEY") == NULL && getenv("SWQOS_API_KEY") == NULL) {
        fprintf(stderr, "set SWQOS_KEY\n");
        return 1;
    }
    if (swqos_open_env() != 0) {
        fprintf(stderr, "swqos_open failed\n");
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
    unlink(SOCK_PATH);
    ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) {
        perror("socket");
        swqos_close();
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0
        || listen(ls, 4) != 0) {
        perror("bind/listen");
        close(ls);
        swqos_close();
        return 1;
    }
    chmod(SOCK_PATH, 0600);
    fprintf(stderr, "SWQOS_RACER ready=%d sock=%s\n", swqos_ready_n(), SOCK_PATH);
    for (;;) {
        int c;
        uint8_t tx[SWQOS_TX_MAX];
        uint16_t len = 0;
        uint8_t hdr[2];
        ssize_t n;
        int rc;
        uint64_t t0, t1;
        uint8_t rep[20];

        c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        n = recv(c, hdr, 2, MSG_WAITALL);
        if (n != 2) {
            close(c);
            continue;
        }
        memcpy(&len, hdr, 2);
        if (len < 64 || len > SWQOS_TX_MAX) {
            close(c);
            continue;
        }
        n = recv(c, tx, len, MSG_WAITALL);
        if (n != (ssize_t)len) {
            close(c);
            continue;
        }
        t0 = now_ns();
        rc = swqos_send(tx, len);
        t1 = now_ns();
        memcpy(rep, &rc, 4);
        memcpy(rep + 4, &t0, 8);
        memcpy(rep + 12, &t1, 8);
        (void)send(c, rep, 20, 0);
        close(c);
        printf("RACER rc=%d send_ns=%" PRIu64 " ready=%d len=%u\n",
               rc, t1 - t0, swqos_ready_n(), (unsigned)len);
        fflush(stdout);
    }
    close(ls);
    unlink(SOCK_PATH);
    swqos_close();
    return 0;
}
