#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

p = Path(sys.argv[1] if len(sys.argv) > 1 else "/home/louis/captures/paper_orbit/sync.bin")
b = p.read_bytes()
print("sync_bytes", len(b))
magic, ver, _pad, hz = struct.unpack_from("<IHHQ", b, 0)
print("magic", hex(magic), "ver", ver, "hz", hz)
off = 16
n = 0
while off + 16 <= len(b):
    ln, kind = struct.unpack_from("<IB", b, off)
    ts = struct.unpack_from("<Q", b, off + 8)[0]
    # rec_begin writes len=12+body but the prefix on disk is 16 bytes
    blen = ln - 12
    if blen < 0 or off + 16 + blen > len(b):
        print("truncated", off, ln)
        break
    body = b[off + 16 : off + 16 + blen]
    if kind == 1 and len(body) >= 20:
        slot, sver, nn = struct.unpack_from("<QQI", body, 0)
        print("INIT slot", slot, "ver", sver, "n", nn)
    elif kind == 2 and len(body) >= 20:
        before, after, pidx = struct.unpack_from("<QQI", body, 0)
        print("MUT", before, "->", after, "pool", pidx)
    elif kind == 3 and len(body) >= 140:
        pidx = struct.unpack_from("<I", body, 64)[0]
        proto = body[68]
        direction = body[69]
        ain, minout, sver, pa, pb, rid, oa, oo, gp = struct.unpack_from(
            "<QQQQQIQQQ", body, 70
        )
        od, ov = body[138], body[139]
        if ov:
            print(
                "DEC valid proto=%u dir=%u pool=%u ver=%u ain=%u aout=%u gp=%u n_ain=%u"
                % (proto, direction, pidx, sver, oa, oo, gp, ain)
            )
            n += 1
    elif kind == 4 and body:
        print("EXEC result", body[-1], "ts", ts)
    off += 16 + blen
print("positive_decisions", n)
