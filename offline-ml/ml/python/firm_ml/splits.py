"""Slot-ordered train / validation / out-of-sample test split.

Boundaries are cut over the sorted distinct slots of all master transactions.
A master transaction has exactly one slot, so all of its rows (labels, pairs)
land in exactly one split.
"""

from __future__ import annotations

import polars as pl

SPLITS = ("train", "val", "test")


def slot_boundaries(slots: pl.Series, fractions: tuple[float, float, float] = (0.70, 0.15, 0.15)) -> dict:
    """Inclusive slot ranges per split."""
    if abs(sum(fractions) - 1.0) > 1e-9:
        raise ValueError("fractions must sum to 1")
    distinct = slots.unique().sort().to_list()
    n = len(distinct)
    cut1 = int(round(n * fractions[0]))
    cut2 = int(round(n * (fractions[0] + fractions[1])))
    if not 0 < cut1 < cut2 < n:
        raise ValueError(f"too few slots ({n}) to split")
    return {
        "train": [distinct[0], distinct[cut1 - 1]],
        "val": [distinct[cut1], distinct[cut2 - 1]],
        "test": [distinct[cut2], distinct[-1]],
        "fractions": list(fractions),
        "distinct_slots": n,
    }


def assign_split(df: pl.DataFrame, bounds: dict, slot_col: str = "slot") -> pl.DataFrame:
    s = pl.col(slot_col)
    expr = (
        pl.when(s <= bounds["train"][1]).then(pl.lit("train"))
        .when(s <= bounds["val"][1]).then(pl.lit("val"))
        .otherwise(pl.lit("test"))
    )
    return df.with_columns(expr.alias("split"))


def check_disjoint(df: pl.DataFrame, key: str = "master_tx_id", slot_col: str = "slot") -> None:
    """Raise if slot ranges overlap or any key appears in two splits."""
    per_split = df.group_by("split").agg(pl.col(slot_col).min().alias("lo"), pl.col(slot_col).max().alias("hi"))
    r = {row["split"]: (row["lo"], row["hi"]) for row in per_split.iter_rows(named=True)}
    order = [s for s in SPLITS if s in r]
    for a, b in zip(order, order[1:]):
        if not r[a][1] < r[b][0]:
            raise AssertionError(f"split {a} {r[a]} overlaps {b} {r[b]}")
    multi = df.group_by(key).agg(pl.col("split").n_unique().alias("n")).filter(pl.col("n") > 1)
    if multi.height:
        raise AssertionError(f"{multi.height} {key} values span more than one split")
