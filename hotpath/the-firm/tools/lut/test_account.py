#!/usr/bin/env python3
from account import parse_account, index_blocked, LUT_NEVER


def main() -> int:
    fail = 0
    raw = bytearray(56 + 64)
    raw[0] = 1
    raw[4:12] = (LUT_NEVER).to_bytes(8, "little")
    raw[20] = 1
    raw[12:20] = (50).to_bytes(8, "little")
    raw[56] = 0xAA
    p = parse_account(bytes(raw))
    if p is None or p["n"] != 2 or p["addrs"][0][0] != 0xAA:
        print("FAIL parse")
        fail += 1
    else:
        print("ok    parse")
    if not index_blocked(p, 50, 1) or index_blocked(p, 50, 0) or index_blocked(p, 51, 1):
        print("FAIL blocked")
        fail += 1
    else:
        print("ok    lifecycle")
    if parse_account(b"\x00" * 10) is not None:
        print("FAIL short")
        fail += 1
    else:
        print("ok    reject short")
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
