#include "tx_features.h"
#include <stddef.h>

_Static_assert(sizeof(txf_header_v1) == 152, "header size");
_Static_assert(sizeof(txf_instruction_v1) == 152, "instruction size");
_Static_assert(sizeof(txf_account_v1) == 56, "account size");
_Static_assert(sizeof(txf_scratch) == TXF_SCRATCH_BYTES, "scratch size");
_Static_assert(offsetof(txf_header_v1, schema_id) == 32, "schema_id offset");
_Static_assert(offsetof(txf_header_v1, version) == 142, "version offset");
_Static_assert(offsetof(txf_encoded_v1, instructions) == 152, "instructions offset");
_Static_assert(sizeof(txf_encoded_v1) == 152 + 152 * TXF_MAX_INSTRUCTIONS + 56 * TXF_MAX_ACCOUNTS,
               "encoded size");

_Static_assert(sizeof(txf_ext_v2) == 24, "ext v2 size");
_Static_assert(offsetof(txf_encoded_v2, ext) == sizeof(txf_encoded_v1), "ext offset");
_Static_assert(sizeof(txf_encoded_v2) == sizeof(txf_encoded_v1) + 24, "encoded v2 size");

int main(void) {
    return 0;
}
