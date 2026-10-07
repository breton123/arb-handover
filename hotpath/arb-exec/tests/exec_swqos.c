#include "swqos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int
main(void)
{
    int rc;

    load_kv(".env");
    load_kv("../.env");
    load_kv("/home/louis/.arb-smoke.env");
    load_kv("/home/louis/.arb-swqos.env");
    load_kv("/home/louis/arb-exec/.deploy/swqos.env");

    if (getenv("SWQOS_KEY") == NULL && getenv("SWQOS_API_KEY") == NULL) {
        fprintf(stderr, "set SWQOS_KEY\n");
        return 1;
    }
    rc = swqos_open_env();
    if (rc != 0) {
        fprintf(stderr, "swqos_open %d\n", rc);
        return 1;
    }
    printf("SWQOS  quic hold  send.swqos.com:11000  ultrasend/1\n");
    printf("       hot path is swqos_send(tx, len)  raw ≤1232  no receipt wait\n");
    swqos_close();
    return 0;
}
