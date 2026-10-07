"""Decision-policy accounting on a hand-built frame."""

import numpy as np
import polars as pl
import pytest

from firm_ml import policy

# Five transactions: two positives, one negative, one sampled UNKNOWN, one unsampled UNKNOWN.
TX = pl.DataFrame({
    "master_tx_id": [1, 2, 3, 4, 5],
    "arb_target": [1, 1, 0, None, None],
    "arb_label": ["POSITIVE", "POSITIVE", "CERTIFIED_NEGATIVE", "UNKNOWN", "UNKNOWN"],
    "relevant": [1, 1, 1, 1, 0],
    "encode_status": [0, 0, 0, 0, 0],
    "arb_score": [0.9, 0.8, 0.7, 0.95, 0.99],
    "pair_sample": [0, 0, 0, 1, 0],
    "min_cover_rank_live": [1, None, None, None, None],
    "n_profitable_routes_seen_in_train": [1, 0, 0, 0, 0],
}, schema_overrides={"master_tx_id": pl.UInt64, "min_cover_rank_live": pl.UInt32, "arb_target": pl.Int8})

# Tx 1: covering pack at catalog rank 1 (B); A is rank 0 and does not cover.
PAIRS = pl.DataFrame({
    "master_tx_id": [1, 1, 2, 3, 4],
    "pack_id": ["A", "B", "A", "A", "A"],
    "pack_rank": [0, 1, 0, 0, 0],
    "eligible_live": [True] * 5,
    "arb_label": ["POSITIVE", "POSITIVE", "POSITIVE", "CERTIFIED_NEGATIVE", "UNKNOWN"],
    "pack_target": [0, 1, 0, 0, None],
    "static": [0.0, -1.0, 0.0, 0.0, 0.0],
    "learned": [0.2, 0.9, 0.5, 0.1, 0.4],
}, schema_overrides={"master_tx_id": pl.UInt64, "pack_rank": pl.UInt32, "pack_target": pl.Int8})


def test_availability_and_depth():
    a = policy.availability(TX, [1, 2], "live")
    assert a["available"].to_list() == [0, 1]  # covering pack has rank 1: available only at k >= 2


def test_ranking_decides_top1():
    assert policy.conditional_topk(PAIRS, "static", 2, (1, 2))["top1"] == 0.0
    assert policy.conditional_topk(PAIRS, "learned", 2, (1, 2))["top1"] == 1.0
    assert policy.conditional_topk(PAIRS, "static", 2, (1, 2))["top2"] == 1.0
    assert policy.conditional_topk(PAIRS, "learned", 1, (1,))["coverable"] == 0  # depth 1 hides pack B


def test_outcomes_follow_pipeline_order_and_weights():
    w = policy.sample_weight(TX, 250_000)  # sampled UNKNOWN counts 4x
    d = policy.per_tx(TX, PAIRS, "learned", 2, 1, "learned")
    r = policy.evaluate(d, "two_stage", 0.75, 1.0, w)
    # Sent: tx1 (pos), tx2 (pos), tx4 (sampled unknown, weight 4). Tx5 fails the gate; tx3 is below 0.75.
    assert r["sends_weighted"] == 1 + 1 + 4
    assert r["succeeded"] == 1 and r["missed"] == {"encoder_unsupported": 0, "relevance_gate": 0, "threshold": 0, "no_covering_pack": 1, "ranking": 0}
    static = policy.evaluate(policy.per_tx(TX, PAIRS, "static", 2, 1, None), "two_stage", 0.75, 1.0, w)
    assert static["missed"]["ranking"] == 1 and static["succeeded"] == 0
    joint = policy.evaluate(d, "joint", 0.5, 1.0, w)
    # Joint scores: tx1 0.9*0.9=0.81, tx2 0.4, tx3 0.07, tx4 0.38 -> only tx1 sent.
    assert joint["sends_weighted"] == 1 and joint["succeeded"] == 1


def test_threshold_for_budget_never_splits_ties():
    s = np.array([0.9, 0.8, 0.8, 0.1, -np.inf])
    w = np.ones(5)
    assert policy.threshold_for_budget(s, w, 1) == 0.9
    assert policy.threshold_for_budget(s, w, 2) == 0.9  # 0.8 is tied twice; taking it would send 3
    assert policy.threshold_for_budget(s, w, 3) == 0.8
    assert policy.threshold_for_budget(s, w, 0.5) == float("inf")


def test_unparsed_positive_is_its_own_outcome():
    tx = TX.with_columns(pl.when(pl.col("master_tx_id") == 2).then(1).otherwise(pl.col("encode_status")).alias("encode_status"),
                         pl.when(pl.col("master_tx_id") == 2).then(0).otherwise(pl.col("relevant")).alias("relevant"))
    d = policy.per_tx(tx, PAIRS, "learned", 2, 1, None)
    r = policy.evaluate(d, "two_stage", 0.75, 1.0, policy.sample_weight(tx, 250_000))
    assert r["missed"]["encoder_unsupported"] == 1 and r["missed"]["relevance_gate"] == 0
