#!/usr/bin/env python3
"""Dump CORE-004 capture files. Recorder writes the same struct the C side reads."""

import argparse
import struct
import sys

MAGIC = 0x43303034
VER = 1
HDR = struct.Struct("<IHHII")

# Keep in lockstep with include/dlmm_cache.h dlmm_cap_rec_t. Offsets are
# printed so a recorder can pack without guessing.


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cap", nargs="?", help="dlmm .cap file")
    ap.add_argument("--layout", action="store_true")
    args = ap.parse_args()
    if args.layout or args.cap is None:
        print("dlmm_cap_hdr_t  16 bytes  magic=C004 ver=1 rec_bytes nrec")
        print("dlmm_cap_rec_t  pool_idx swap_for_y amount_in min_out now_ts")
        print("                before_active after_active bin_step")
        print("                nbin_before nbin_after status crossed")
        print("                parameters v_before v_after")
        print("                before_rx/ry after_rx/ry")
        print("                bins_before[64] bins_after[64]  (id, x, y)")
        print("economically compared: active_id reserves vol_acc touched bin amounts")
        if args.cap is None:
            return 0
    with open(args.cap, "rb") as f:
        raw = f.read()
    if len(raw) < HDR.size:
        print("short file", file=sys.stderr)
        return 1
    magic, ver, rec_bytes, nrec, _ = HDR.unpack_from(raw, 0)
    if magic != MAGIC or ver != VER:
        print(f"bad header magic={magic:#x} ver={ver}", file=sys.stderr)
        return 1
    print(f"{args.cap}: {nrec} records × {rec_bytes} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
