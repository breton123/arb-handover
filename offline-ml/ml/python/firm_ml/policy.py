"""Decision-policy evaluation over a frozen candidate catalog.

A transaction is *sent* with its top-L packs. Two decision rules:

- two-stage: relevant & arb_score >= t & at least one eligible live pack; packs ranked by a pack score
- joint:     relevant & max over eligible live packs of arb_score * pack_prob >= t; packs ranked by pack_prob

Packs are eligible at depth k when their catalog rank within the key is < k. Volume counts every
labelled transaction and the deterministic UNKNOWN pair sample, weighted by 1 / sample rate, so both
rules are measured on the same population.

Each POSITIVE transaction gets exactly one outcome, in pipeline order:
encoder_unsupported (framed-tx-v1 cannot parse it, e.g. v1 wire format) -> relevance_gate ->
threshold (classifier or joint score) -> no_covering_pack (at depth k) -> ranking -> success.
"""

from __future__ import annotations

import numpy as np
import polars as pl

from .metrics import wilson

KEY = "master_tx_id"


def sample_weight(tx: pl.DataFrame, unknown_pair_ppm: int) -> pl.Expr:
    """1 for labelled rows, 1e6/ppm for sampled UNKNOWN rows, 0 otherwise."""
    rate = unknown_pair_ppm / 1e6
    return (
        pl.when(pl.col("arb_target").is_not_null()).then(1.0)
        .when(pl.col("pair_sample") == 1).then(1.0 / rate if rate > 0 else 0.0)
        .otherwise(0.0)
    )


def availability(tx: pl.DataFrame, ks, mode: str = "live") -> pl.DataFrame:
    """Share of POSITIVE transactions with a covering eligible pack at depth k."""
    pos = tx.filter(pl.col("arb_target") == 1)
    n = pos.height
    rows = []
    for k in ks:
        a = int((pos[f"min_cover_rank_{mode}"] < k).sum())
        rows.append({"k": k, "positives": n, "available": a, "availability": a / n if n else None,
                     "ci": wilson(a, n)})
    return pl.DataFrame(rows)


def ranked(pairs: pl.DataFrame, score: str, depth: int) -> pl.DataFrame:
    """Live pairs at depth, with `sel` = 1 for the best pack per transaction.

    Ties break on catalog rank, then pack_id, so every scorer is deterministic."""
    p = pairs.filter(pl.col("eligible_live") & (pl.col("pack_rank") < depth))
    return p.sort([KEY, score, "pack_rank", "pack_id"], descending=[False, True, False, False]).with_columns(
        pl.int_range(1, pl.len() + 1).over(KEY).alias("sel")
    )


def conditional_topk(pairs: pl.DataFrame, score: str, depth: int, Ls=(1, 2, 3)) -> dict:
    """Top-L pack success over POSITIVE transactions that have a covering live pack at this depth."""
    r = ranked(pairs.filter(pl.col("arb_label") == "POSITIVE"), score, depth)
    per = r.group_by(KEY).agg(
        pl.col("pack_target").max().alias("coverable"),
        *[(pl.col("pack_target") * (pl.col("sel") <= L)).max().alias(f"top{L}") for L in Ls],
        pl.col("sel").filter(pl.col("pack_target") == 1).min().alias("first_hit"),
        pl.len().alias("eligible"),
    ).filter(pl.col("coverable") == 1)
    out = {"depth": depth, "scorer": score, "coverable": per.height,
           "mean_eligible": float(per["eligible"].mean()) if per.height else None}
    for L in Ls:
        hits = int(per[f"top{L}"].sum()) if per.height else 0
        out[f"top{L}"] = hits / per.height if per.height else None
        out[f"top{L}_ci"] = wilson(hits, per.height)
    out["mrr"] = float((1.0 / per["first_hit"]).mean()) if per.height else None
    return out


def per_tx(tx: pl.DataFrame, pairs: pl.DataFrame, score: str, depth: int, L: int, joint_prob: str | None,
           ranked_pairs: pl.DataFrame | None = None) -> pl.DataFrame:
    """One row per transaction: decision score for both rules and whether the top-L contains a covering pack.

    `ranked_pairs` (= `ranked(pairs, score, depth)`) skips the sort when several L share one depth."""
    r = ranked(pairs, score, depth) if ranked_pairs is None else ranked_pairs
    agg = [
        (pl.col("pack_target").fill_null(0) * (pl.col("sel") <= L)).max().cast(pl.Int8).alias("hit"),
        pl.len().alias("n_pairs"),
    ]
    if joint_prob:
        agg.append((pl.col(joint_prob) * pl.col("arb_score")).max().alias("joint"))
    by = r.join(tx.select(KEY, "arb_score"), on=KEY, how="left").group_by(KEY).agg(agg)
    out = tx.join(by, on=KEY, how="left").with_columns(
        pl.col("hit").fill_null(0), pl.col("n_pairs").fill_null(0),
        (pl.col("min_cover_rank_live") < depth).fill_null(False).alias("coverable"),
    )
    if not joint_prob:
        out = out.with_columns(pl.lit(None, dtype=pl.Float64).alias("joint"))
    return out


def decision_score(d: pl.DataFrame, rule: str) -> pl.Series:
    """Score that the threshold applies to; -inf when the rule can never send."""
    gate = pl.col("relevant") == 1
    if rule == "two_stage":
        expr = pl.when(gate & (pl.col("n_pairs") > 0)).then(pl.col("arb_score"))
    elif rule == "joint":
        expr = pl.when(gate & (pl.col("n_pairs") > 0)).then(pl.col("joint"))
    else:
        raise ValueError(rule)
    return d.select(expr.otherwise(float("-inf")).fill_null(float("-inf"))).to_series()


def threshold_for_budget(scores: np.ndarray, weights: np.ndarray, budget: float) -> float:
    """Lowest threshold whose weighted send count stays <= budget (inf if even one send exceeds it)."""
    ok = np.isfinite(scores) & (weights > 0)
    s, w = scores[ok], weights[ok]
    order = np.argsort(-s, kind="stable")
    s, cum = s[order], np.cumsum(w[order])
    fits = np.where(cum <= budget)[0]
    if not len(fits):
        return float("inf")
    i = fits[-1]
    # Do not split a tie: drop back to the last strictly higher score if the tie would exceed the budget.
    while i + 1 < len(s) and s[i + 1] == s[i]:
        i -= 1
        if i < 0:
            return float("inf")
    return float(s[i])


def evaluate(d: pl.DataFrame, rule: str, threshold: float, hours: float, weight: pl.Expr) -> dict:
    """Send volume, precision, recall and the POSITIVE outcome decomposition at one threshold."""
    d = d.with_columns(decision_score(d, rule).alias("_score"), weight.alias("_w"))
    sent = pl.col("_score") >= threshold
    s = d.filter(sent)
    lab = s.filter(pl.col("arb_target").is_not_null())
    pos = d.filter(pl.col("arb_target") == 1)
    encoded = pl.col("encode_status") == 0
    gate = encoded & (pl.col("relevant") == 1)
    score_ok = (pl.col("arb_score") if rule == "two_stage" else pl.col("joint")).fill_null(float("-inf")) >= threshold
    n = lambda e: pos.filter(e).height
    missed = {
        "encoder_unsupported": n(~encoded),
        "relevance_gate": n(encoded & ~gate),
        "threshold": n(gate & ~score_ok),
        "no_covering_pack": n(gate & score_ok & ~pl.col("coverable")),
        "ranking": n(gate & score_ok & pl.col("coverable") & (pl.col("hit") == 0)),
    }
    success = n(gate & score_ok & pl.col("coverable") & (pl.col("hit") == 1))
    sent_pos = n(sent)
    reached = n(gate & score_ok & pl.col("coverable"))
    weighted = float(s["_w"].sum())
    return {
        "rule": rule,
        "threshold": threshold,
        "sends_weighted": weighted,
        "sends_per_chain_hour": weighted / hours if hours else None,
        "sends_labelled": lab.height,
        "unknown_share_of_sends": 1 - (lab.height / weighted) if weighted else None,
        "precision_arb": float(lab["arb_target"].mean()) if lab.height else None,
        "precision_capture": float((lab["hit"] * lab["arb_target"]).mean()) if lab.height else None,
        "positives": pos.height,
        "positive_recall": sent_pos / pos.height if pos.height else None,
        "availability": n(pl.col("coverable")) / pos.height if pos.height else None,
        "topL_success_given_sent_and_available": success / reached if reached else None,
        "end_to_end_recall": success / pos.height if pos.height else None,
        "end_to_end_recall_ci": wilson(success, pos.height),
        "succeeded": success,
        "missed": missed,
    }


def conditional_success(d: pl.DataFrame, rule: str, threshold: float) -> pl.DataFrame:
    """End-to-end success of POSITIVE transactions by gate visibility, pack availability and route recurrence."""
    score = (pl.col("arb_score") if rule == "two_stage" else pl.col("joint")).fill_null(float("-inf"))
    pos = d.filter(pl.col("arb_target") == 1).with_columns(
        ((pl.col("encode_status") == 0) & (pl.col("relevant") == 1) & (score >= threshold) & pl.col("coverable")
         & (pl.col("hit") == 1)).alias("success"),
        pl.when(pl.col("encode_status") != 0).then(pl.lit("unparsed"))
        .when(pl.col("relevant") == 1).then(pl.lit("raw-visible")).otherwise(pl.lit("ALT-only")).alias("gate_visibility"),
        (pl.col("n_profitable_routes_seen_in_train") > 0).alias("route_seen_in_train"),
    )
    rows = []
    for name, cond in [("all", pl.lit(True)), ("covering pack exists", pl.col("coverable")),
                       ("route seen in train", pl.col("route_seen_in_train")),
                       ("route not seen in train", ~pl.col("route_seen_in_train")),
                       ("raw-visible (gate passes)", pl.col("gate_visibility") == "raw-visible"),
                       ("ALT-only (gate fails)", pl.col("gate_visibility") == "ALT-only"),
                       ("encoder cannot parse (v1 format)", pl.col("gate_visibility") == "unparsed")]:
        g = pos.filter(cond)
        k = int(g["success"].sum())
        rows.append({"subset": name, "positives": g.height, "succeeded": k,
                     "rate": k / g.height if g.height else None, "ci": wilson(k, g.height)})
    return pl.DataFrame(rows)
