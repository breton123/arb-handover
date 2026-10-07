"""Program-derived address. Same Edwards check as arb-cap record_dlmm."""
from __future__ import annotations

import hashlib

_P = 2**255 - 19
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_B58 = b"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58decode(s: str) -> bytes:
    n = 0
    for c in s.encode("ascii"):
        n = n * 58 + _B58.index(c)
    raw = n.to_bytes((n.bit_length() + 7) // 8 or 1, "big")
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    return b"\x00" * pad + raw.lstrip(b"\x00") or b"\x00"


def b58encode(raw: bytes) -> str:
    n = int.from_bytes(raw, "big")
    out = bytearray()
    while n > 0:
        n, r = divmod(n, 58)
        out.append(_B58[r])
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    return (b"1" * pad + out[::-1]).decode("ascii")


def _on_curve(pt: bytes) -> bool:
    if len(pt) != 32:
        return False
    y = int.from_bytes(pt, "little") & ((1 << 255) - 1)
    if y >= _P:
        return False
    y2 = pow(y, 2, _P)
    u = (y2 - 1) % _P
    v = (_D * y2 + 1) % _P
    x2 = (u * pow(v, _P - 2, _P)) % _P
    return pow(x2, (_P - 1) // 2, _P) != _P - 1


def find_pda(seeds: list[bytes], program: bytes) -> bytes:
    for bump in range(255, -1, -1):
        h = hashlib.sha256()
        for s in seeds:
            h.update(s)
        h.update(bytes([bump]))
        h.update(program)
        h.update(b"ProgramDerivedAddress")
        digest = h.digest()
        if not _on_curve(digest):
            return digest
    raise RuntimeError("no pda")


def pump_global_pk() -> str:
    from layout import PUMP_AMM

    return b58encode(find_pda([b"global_config"], b58decode(PUMP_AMM)))
