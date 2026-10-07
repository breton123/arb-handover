#!/usr/bin/env python3
import sys
from pathlib import Path

src = Path(sys.argv[1])
n = int(sys.argv[2])
out = Path(sys.argv[3])
names = sorted(p.name for p in src.glob("*.cap"))[:n]
out.write_text("".join(f"{src / name}\n" for name in names), encoding="utf-8")
print(len(names), "files ->", out)
