import polars as pl
import pytest

from firm_ml import splits


def synthetic(n_slots=100, per_slot=3):
    return pl.DataFrame({
        "slot": [1000 + s for s in range(n_slots) for _ in range(per_slot)],
        "master_tx_id": list(range(n_slots * per_slot)),
    })


def test_boundaries_are_ordered_and_cover_all_slots():
    df = synthetic()
    b = splits.slot_boundaries(df["slot"])
    assert b["train"] == [1000, 1069] and b["val"] == [1070, 1084] and b["test"] == [1085, 1099]
    out = splits.assign_split(df, b)
    splits.check_disjoint(out)
    assert dict(out.group_by("split").len().iter_rows()) == {"train": 210, "val": 45, "test": 45}


def test_overlap_and_split_transactions_are_detected():
    df = splits.assign_split(synthetic(), splits.slot_boundaries(synthetic()["slot"]))
    bad = df.with_columns(
        pl.when(pl.col("slot") == 1099).then(pl.lit("train")).otherwise(pl.col("split")).alias("split"))
    with pytest.raises(AssertionError):
        splits.check_disjoint(bad)
    dup = pl.concat([df, df.head(1).with_columns(pl.lit("test").alias("split"))])
    with pytest.raises(AssertionError):
        splits.check_disjoint(dup)


def test_real_split_is_disjoint(tx):
    b = splits.slot_boundaries(tx["slot"])
    out = splits.assign_split(tx.select("master_tx_id", "slot"), b)
    splits.check_disjoint(out)
    assert b["train"][1] < b["val"][0] <= b["val"][1] < b["test"][0]
