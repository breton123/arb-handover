#include "route0.h"
#include "tx_template.h"

#include <stdio.h>
#include <string.h>

/* EXEC-LIVE-002 — compile the frozen template from a live 35-key vector.
 * Does not mutate route0_tx_compile. keys.bin = 35*32, exec.bin = 32.
 */

int
main(int argc, char **argv)
{
    uint8_t keys[ROUTE0_N * 32];
    uint8_t exec[32];
    route0_tx_template_t tmpl;
    FILE *f;
    const char *kpath;
    const char *epath;
    const char *opath;
    size_t n;
    uint32_t i;

    if (argc != 4) {
        fprintf(stderr, "usage: exec_live002 keys.bin exec.bin out.tx\n");
        return 1;
    }
    kpath = argv[1];
    epath = argv[2];
    opath = argv[3];

    f = fopen(kpath, "rb");
    if (f == NULL) {
        perror(kpath);
        return 1;
    }
    n = fread(keys, 1, sizeof(keys), f);
    fclose(f);
    if (n != sizeof(keys)) {
        fprintf(stderr, "keys.bin want %zu got %zu\n", sizeof(keys), n);
        return 1;
    }

    f = fopen(epath, "rb");
    if (f == NULL) {
        perror(epath);
        return 1;
    }
    n = fread(exec, 1, 32, f);
    fclose(f);
    if (n != 32) {
        fprintf(stderr, "exec.bin want 32 got %zu\n", n);
        return 1;
    }

    if (route0_tx_compile(keys, exec, ROUTE0_CU_LIMIT, &tmpl) != 0) {
        fprintf(stderr, "route0_tx_compile rejected the live vector\n");
        return 1;
    }

    f = fopen(opath, "wb");
    if (f == NULL) {
        perror(opath);
        return 1;
    }
    if (fwrite(tmpl.bytes, 1, ROUTE0_TX_LEN, f) != ROUTE0_TX_LEN) {
        fprintf(stderr, "write template\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("EXEC-LIVE-002  compile ok  len=%u  nkeys=%u\n",
           ROUTE0_TX_LEN, ROUTE0_TX_NKEYS);
    for (i = 0; i < ROUTE0_N; i++) {
        printf("  [%02u] %s\n", i, route0_acc_name[i]);
    }
    return 0;
}
