"""Loaders keep feature order and targets exactly as ml-prep wrote them."""

import polars as pl
import pyarrow.parquet as pq
import pytest

from firm_ml import data, splits


def test_feature_order_matches_manifest_and_files(prep, tx):
    schema = pq.read_schema(prep.root / "tx_features" / prep.manifest["labels"]["parts"][0])
    file_order = [f.name for f in schema if (f.metadata or {}).get(b"role") == b"feature"]
    assert file_order == prep.tx_features
    assert tx.columns[-len(prep.tx_features):] == prep.tx_features
    packs = data.load_packs(prep)
    assert [c for c in packs.columns if c in prep.pack_features] == prep.pack_features


def test_targets_preserved(prep, tx):
    summary = prep.manifest["labels"]
    counts = dict(tx.group_by("arb_label").len().iter_rows())
    assert counts.get("POSITIVE", 0) == summary["tx_positive"]
    assert counts.get("CERTIFIED_NEGATIVE", 0) == summary["tx_certified_negative"]
    assert counts.get("UNKNOWN", 0) == summary["tx_unknown"]
    expect = {"POSITIVE": 1, "CERTIFIED_NEGATIVE": 0}
    for label, target in tx.select("arb_label", "arb_target").unique().iter_rows():
        assert target == expect.get(label), (label, target)


def test_unknown_never_becomes_negative(prep, tx):
    unknown = tx.filter(pl.col("arb_label") == "UNKNOWN")
    assert unknown["arb_target"].null_count() == unknown.height
    train = data.arb_training_rows(tx)
    assert (train["arb_label"] != "UNKNOWN").all()
    pairs = data.load_pairs(prep)
    unk = pairs.filter(pl.col("arb_label") == "UNKNOWN")
    assert unk["pack_target"].null_count() == unk.height
    lab = pairs.filter(pl.col("arb_label") != "UNKNOWN")
    assert lab["pack_target"].null_count() == 0
    assert int((pairs["pack_target"] == 1).sum()) == prep.manifest["pairs"]["positive_target"]
    neg = lab.filter(pl.col("arb_label") == "CERTIFIED_NEGATIVE")
    assert neg.height == 0 or neg["pack_target"].max() == 0


def test_ids_labels_and_meta_are_refused_as_features(prep):
    data.check_feature_columns(prep, prep.tx_features + prep.pack_features)
    for col in sorted(prep.forbidden):
        with pytest.raises(ValueError):
            data.check_feature_columns(prep, [col])
    with pytest.raises(ValueError):
        data.check_feature_columns(prep, ["not_a_feature"])
    assert not set(prep.tx_features + prep.pack_features) & prep.forbidden
    train_meta = set(prep.pack_features_in("train_meta"))
    for name in prep.tx_features + prep.pack_features:
        assert not any(w in name for w in ("label", "target", "profit", "ppm", "objective")), name
        # Training-coverage counts are allowed only as frozen library train_meta (window checked below).
        if any(w in name for w in ("coverage", "covered", "rank", "support", "gain")):
            assert name in train_meta, name
    if train_meta:
        assert prep.manifest["packs"]["train_master_tx_id_max"] is not None


def test_ranker_frame_keeps_rows_and_split(prep, tx):
    tx = splits.assign_split(tx, splits.slot_boundaries(tx["slot"]))
    pairs = data.load_pairs(prep)
    rf = data.ranker_frame(prep, tx, pairs, data.load_packs(prep))
    assert rf.height == pairs.height
    splits.check_disjoint(rf)
    joined = rf.join(tx.select("master_tx_id", pl.col("split").alias("tx_split")), on="master_tx_id")
    assert (joined["split"] == joined["tx_split"]).all()


def test_gate_columns_are_loaded_but_never_features(prep, tx):
    assert prep.gate_columns == ["rel_market_writable", "rel_market_readonly", "rel_venue_program", "relevant"]
    for c in prep.gate_columns:
        assert c in tx.columns and c not in prep.tx_features
        with pytest.raises(ValueError):
            data.check_feature_columns(prep, [c])
    assert set(tx["relevant"].unique().to_list()) <= {0, 1}
    assert int(tx["relevant"].sum()) == prep.manifest["features"]["relevant"]


def test_packs_are_frozen_to_the_train_split(prep, tx):
    window = prep.manifest["packs"]
    out = splits.assign_split(tx.select("master_tx_id", "slot"), splits.slot_boundaries(tx["slot"]))
    train_ids = out.filter(pl.col("split") == "train")["master_tx_id"]
    assert train_ids.min() <= window["train_master_tx_id_min"]
    assert window["train_master_tx_id_max"] <= train_ids.max()
