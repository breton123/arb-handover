#include "oracle/soak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    oracle_soak_stats_t st;
    const char *path = NULL;
    const char *root;
    int scale = 0;

    if (argc >= 3 && strcmp(argv[1], "--jsonl") == 0) {
        path = argv[2];
    }
    if (oracle_admit_jsonl(path, &st) != 0) {
        fprintf(stderr, "PUMP-ORACLE-002 jsonl not found\n");
        return 1;
    }
    oracle_soak_print(&st);
    root = getenv("FIRM_SOAK_ROOT");
    if (root != NULL && root[0] != '\0') {
        scale = 1;
    }
    if (st.usable == 0) {
        fprintf(stderr, "no usable Pump rows\n");
        return 1;
    }
    if (st.unexplained != 0 || st.exact_wrong != 0) {
        fprintf(stderr,
                "UNEXPLAINED=%u exact_wrong=%u — admission failed, DLMM blocked\n",
                st.unexplained, st.exact_wrong);
        return 1;
    }
    if (scale && st.usable < 1000) {
        fprintf(stderr,
                "FIRM_SOAK_ROOT set but usable=%u < 1000 — import the soak tree\n",
                st.usable);
        return 1;
    }
    if (!scale && st.usable < 1000) {
        printf("note: soak tree not mounted; seed corpus only (%u rows). "
               "Set FIRM_SOAK_ROOT to run the 4118-file gate.\n",
               st.usable);
    } else {
        printf("scale gate: 0 UNEXPLAINED on %u usable Pump rows\n", st.usable);
    }
    return 0;
}
