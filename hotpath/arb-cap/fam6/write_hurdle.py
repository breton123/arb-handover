#!/usr/bin/env python3
"""Write per-sequence hurdles from proven Custom(6) CU plus conservative estimates."""
import json
from pathlib import Path

H = Path("/home/louis/arb-core/include/hops_hurdle.h")
OUT = Path("/home/louis/arb-cap/fam6/HURDLE.json")
SWQOS, SAFETY, SIG, PRICE = 150_000, 50_000, 5_000, 1_000_000

# Proven this session against CnddPhKV... / ARBHOPS0.
measured = {
    "dlmm-dlmm": 87236,
    "dlmm-pump": 135673,
    "pump-dlmm": 149534,
}
# Not yet Custom(6) on this program id. Conservative until they are.
estimate = {
    "pump-pump": 180000,
    "dlmm-dlmm-dlmm": 160000,
    "dlmm-dlmm-pump": 200000,
    "pump-dlmm-dlmm": 200000,
}

rows = []
for seq in (
    "dlmm-dlmm", "dlmm-pump", "pump-dlmm", "pump-pump",
    "dlmm-dlmm-dlmm", "dlmm-dlmm-pump", "pump-dlmm-dlmm",
):
    cu = measured.get(seq)
    src = "measured" if cu else "estimate"
    cu = cu or estimate[seq]
    lim = int(cu * 12 / 10)
    onchain = SIG + (lim * PRICE) // 1_000_000
    rows.append({
        "seq": seq,
        "cu": cu if src == "measured" else None,
        "cu_used": cu,
        "cu_limit": lim,
        "cu_price": PRICE,
        "onchain_fee": onchain,
        "swqos_fee": SWQOS,
        "safety": SAFETY,
        "min_gross": onchain + SWQOS + SAFETY,
        "source": src,
    })

lines = [
    "#ifndef ARB_CORE_HOPS_HURDLE_H",
    "#define ARB_CORE_HOPS_HURDLE_H",
    "",
    "#include <stdint.h>",
    "#include <string.h>",
    "",
    "#define HOPS_SWQOS_FEE   150000ull",
    "#define HOPS_SAFETY_FEE   50000ull",
    "#define HOPS_SIG_FEE       5000ull",
    "#define HOPS_CU_PRICE   1000000ull",
    "",
    "static inline uint64_t hops_min_gross(uint64_t cu_limit)",
    "{",
    "    return HOPS_SIG_FEE + (cu_limit * HOPS_CU_PRICE) / 1000000ull",
    "        + HOPS_SWQOS_FEE + HOPS_SAFETY_FEE;",
    "}",
    "",
    "static inline uint64_t hops_hurdle_for_seq(const char *seq)",
    "{",
    "    if (seq == NULL) {",
    "        return hops_min_gross(200000ull);",
    "    }",
]
for r in rows:
    lines.append(f'    if (strcmp(seq, "{r["seq"]}") == 0) {{')
    lines.append(f"        return hops_min_gross({r['cu_limit']}ull);")
    lines.append("    }")
lines += [
    "    return hops_min_gross(200000ull);",
    "}",
    "",
    "#endif",
    "",
]
H.write_text("\n".join(lines), encoding="utf-8")
OUT.write_text(json.dumps({"program": "CnddPhKV1fnKE7ic5nSJcoVq2XFmQ9daE3tuqc2u6qTt", "rows": rows}, indent=2) + "\n")
print("HURDLE")
for r in rows:
    print(f"  {r['seq']:16} {r['source']:9} cu={r['cu_used']} lim={r['cu_limit']} min_gross={r['min_gross']}")
