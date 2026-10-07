#!/usr/bin/env python3
"""Count typed SOL-closed 2-hop / 3-hop routes. DLMM+Pump only. No send."""
from __future__ import annotations

SOL = "So11111111111111111111111111111111111111112"


def _edges(pools: list[dict]) -> list[tuple[str, str, str]]:
    out = []
    for p in pools:
        kind = p.get("kind") or p.get("proto") or ""
        if kind not in ("dlmm", "pump"):
            continue
        mx, my = p.get("mx") or "", p.get("my") or ""
        if not mx or not my or mx == my:
            continue
        out.append((kind, mx, my))
    return out


def _adj(edges: list[tuple[str, str, str]]) -> dict[str, list[tuple[int, str, str, int]]]:
    tab: dict[str, list[tuple[int, str, str, int]]] = {}
    for i, (kind, mx, my) in enumerate(edges):
        tab.setdefault(mx, []).append((i, my, kind, 0))
        tab.setdefault(my, []).append((i, mx, kind, 1))
    return tab


def _route0(e1: tuple[str, str, str], e2: tuple[str, str, str]) -> bool:
    kinds = {e1[0], e2[0]}
    if kinds != {"dlmm", "pump"}:
        return False
    d = e1 if e1[0] == "dlmm" else e2
    p = e1 if e1[0] == "pump" else e2
    return d[2] == SOL and p[2] == SOL and d[1] == p[1] and d[1] != SOL


def count_closed(pools: list[dict]) -> dict[str, int]:
    edges = _edges(pools)
    tab = _adj(edges)
    n2 = n3 = r0 = f5 = 0
    for i1, t1, k1, _d1 in tab.get(SOL, []):
        if t1 == SOL:
            continue
        for i2, t2, k2, _d2 in tab.get(t1, []):
            if i2 == i1:
                continue
            if t2 == SOL:
                n2 += 1
                pair = (edges[i1], edges[i2])
                if _route0(*pair):
                    r0 += 1
                elif {k1, k2} == {"dlmm", "pump"}:
                    f5 += 1
                continue
            if t2 == t1:
                continue
            for i3, t3, _k3, _d3 in tab.get(t2, []):
                if i3 == i1 or i3 == i2:
                    continue
                if t3 == SOL:
                    n3 += 1
    return {"n2": n2, "n3": n3, "route0": r0, "fam5": f5, "closed": n2 + n3}


def exec_support(delta: dict[str, int]) -> float:
    if delta.get("route0", 0) > 0:
        return 2.0
    if delta.get("n3", 0) > 0:
        return 1.5
    if delta.get("fam5", 0) > 0 or delta.get("n2", 0) > 0:
        return 1.0
    return 0.0


def delta_closed(base: list[dict], extra: list[dict]) -> dict[str, int]:
    b = count_closed(base)
    a = count_closed(base + extra)
    return {k: a[k] - b[k] for k in a}
