#include "route_fam.h"
#include "route0.h"

#include <string.h>

int
route_fam_init(route_fam_t *f)
{
    uint8_t keys[ROUTE0_N][32];
    uint8_t exec[32];
    uint32_t i;
    uint8_t fam;

    if (f == NULL) {
        return -1;
    }
    memset(f, 0, sizeof(*f));
    memset(keys, 0, sizeof(keys));
    memset(exec, 0, sizeof(exec));
    for (i = 0; i < ROUTE0_N; i++) {
        keys[i][0] = 0xA0;
        keys[i][1] = (uint8_t)i;
    }
    memcpy(keys[ROUTE0_ACC_PUMP_BASE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_X_MINT], 32);
    memcpy(keys[ROUTE0_ACC_PUMP_QUOTE_MINT],
           keys[ROUTE0_ACC_DLMM_TOKEN_Y_MINT], 32);
    exec[0] = 0xE0;
    for (fam = 0; fam < ROUTE_FAM_N; fam++) {
        if (route0_tx_compile(&keys[0][0], exec, ROUTE0_CU_LIMIT,
                              &f->tmpl[fam]) != 0) {
            return -1;
        }
        f->have[fam] = 1;
    }
    return 0;
}
