"""Classifier, ranker and combined-system metrics."""

from __future__ import annotations

import numpy as np
import polars as pl
from sklearn.metrics import average_precision_score, precision_recall_curve, roc_auc_score

# ---------------------------------------------------------------- classifier


def classifier_summary(y: np.ndarray, p: np.ndarray) -> dict:
    y = np.asarray(y)
    out = {"n": int(len(y)), "positives": int(y.sum()), "base_rate": float(y.mean()) if len(y) else None}
    if 0 < y.sum() < len(y):
        out["pr_auc"] = float(average_precision_score(y, p))
        out["roc_auc"] = float(roc_auc_score(y, p))
    return out


def threshold_for_precision(y: np.ndarray, p: np.ndarray, target: float, min_positives: int = 1) -> float | None:
    """Lowest threshold whose precision >= target with at least `min_positives`
    true positives (i.e. the highest-recall operating point meeting the target)."""
    prec, rec, thr = precision_recall_curve(y, p)
    tp = rec[:-1] * y.sum()
    ok = np.where((prec[:-1] >= target) & (tp >= min_positives))[0]
    return float(thr[ok[0]]) if len(ok) else None


def at_threshold(y: np.ndarray, p: np.ndarray, t: float) -> dict:
    pred = p >= t
    tp = int((pred & (y == 1)).sum())
    fp = int((pred & (y == 0)).sum())
    fn = int((~pred & (y == 1)).sum())
    tn = int((~pred & (y == 0)).sum())
    return {
        "threshold": float(t),
        "tp": tp, "fp": fp, "fn": fn, "tn": tn,
        "precision": tp / (tp + fp) if tp + fp else None,
        "recall": tp / (tp + fn) if tp + fn else None,
        "selected": tp + fp,
    }


def calibration_table(y: np.ndarray, p: np.ndarray, bins: int = 10) -> pl.DataFrame:
    """Quantile bins of predicted probability vs observed rate."""
    df = pl.DataFrame({"y": y, "p": p})
    if df.height == 0:
        return df
    df = df.with_columns(pl.col("p").qcut(bins, labels=None, allow_duplicates=True).alias("bin"))
    return df.group_by("bin").agg(
        pl.len().alias("n"), pl.col("p").mean().alias("mean_pred"), pl.col("y").mean().alias("observed")
    ).sort("mean_pred")


def wilson(k: int, n: int, z: float = 1.96) -> tuple[float, float] | None:
    """95% Wilson interval for k successes out of n. OOS positives are few; read these."""
    if n == 0:
        return None
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * np.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (float(c - h), float(c + h))


def selectivity_table(y: np.ndarray, p: np.ndarray, top_fracs=(0.5, 0.2, 0.1, 0.05, 0.02, 0.01)) -> pl.DataFrame:
    """Precision/recall when sending only the top fraction of scored transactions."""
    order = np.argsort(-p, kind="stable")
    rows = []
    for f in top_fracs:
        n = max(1, int(round(len(p) * f)))
        sel = order[:n]
        tp = int(y[sel].sum())
        rows.append({"top_frac": f, "sent": n, "min_score": float(p[sel].min()), "tp": tp,
                     "precision": tp / n, "recall": tp / max(1, int(y.sum()))})
    return pl.DataFrame(rows)


def chain_hours(block_time: pl.Series) -> float:
    bt = block_time.drop_nulls()
    if bt.len() < 2:
        return float("nan")
    return float(bt.max() - bt.min()) / 3600.0


# -------------------------------------------------------------------- ranker


def rank_within(df: pl.DataFrame, score: str, key: str = "master_tx_id") -> pl.DataFrame:
    """Add `rank` (1 = best) per transaction. Ties break by pack_id for determinism."""
    return df.sort([key, score, "pack_id"], descending=[False, True, False]).with_columns(
        pl.int_range(1, pl.len() + 1).over(key).alias("rank")
    )


def topk_coverage(df: pl.DataFrame, score: str, ks=(1, 2, 3), key: str = "master_tx_id", target: str = "pack_target") -> dict:
    """Per transaction with at least one covering eligible pack: does top-k contain one?"""
    r = rank_within(df, score, key)
    per_tx = r.group_by(key).agg(
        pl.col(target).max().alias("coverable"),
        *[(pl.col(target) * (pl.col("rank") <= k)).max().alias(f"top{k}") for k in ks],
        pl.col("rank").filter(pl.col(target) == 1).min().alias("first_hit"),
        pl.len().alias("n_eligible"),
    )
    cov = per_tx.filter(pl.col("coverable") == 1)
    out = {"transactions": per_tx.height, "coverable": cov.height}
    for k in ks:
        out[f"top{k}"] = float(cov[f"top{k}"].mean()) if cov.height else None
    out["mrr"] = float((1.0 / cov["first_hit"]).mean()) if cov.height else None
    out["mean_eligible"] = float(cov["n_eligible"].mean()) if cov.height else None
    return out


# -------------------------------------------------------------------- system


def funnel(
    tx: pl.DataFrame,
    pairs_scored: pl.DataFrame,
    threshold: float,
    k: int,
    eligible_col: str,
    hours: float,
    score: str = "rank_score",
    gate_col: str | None = "relevant",
) -> dict:
    """End-to-end on one split: relevance gate -> arb threshold -> eligible packs -> top-k.

    `tx` holds every master transaction of the split (labelled or not) with
    arb_score, the gate column and n_eligible/n_covering_<mode>. `pairs_scored`
    holds the ranker score, filtered here to `eligible_col` packs.

    Each POSITIVE transaction gets exactly one outcome, checked in this order:
    no covering pack (pack availability) -> relevance gate miss -> classifier
    miss -> ranker miss (no covering pack in top-k) -> success.
    """
    mode = "live" if eligible_col == "eligible_live" else "oracle"
    cov_col, elig_col = f"n_covering_{mode}", f"n_eligible_{mode}"
    gate = (pl.col(gate_col) == 1) if gate_col else pl.lit(True)
    passed = gate & (pl.col("arb_score") >= threshold)

    pairs = pairs_scored.filter(pl.col(eligible_col))
    hit = pl.DataFrame(schema={"master_tx_id": pl.UInt64, "hit": pl.Int8})
    if pairs.height:
        r = rank_within(pairs, score)
        hit = r.filter(pl.col("rank") <= k).group_by("master_tx_id").agg(pl.col("pack_target").max().cast(pl.Int8).alias("hit"))

    pos = tx.filter(pl.col("arb_target") == 1).join(hit, on="master_tx_id", how="left").with_columns(pl.col("hit").fill_null(0))
    has_pack = pl.col(cov_col) > 0
    n = lambda e: pos.filter(e).height
    missed = {
        "no_candidate_pack": n(~has_pack),
        "relevance_gate": n(has_pack & ~gate),
        "classifier": n(has_pack & gate & (pl.col("arb_score") < threshold)),
        "ranker": n(has_pack & passed & (pl.col("hit") == 0)),
    }
    success = n(has_pack & passed & (pl.col("hit") == 1))
    with_pack, reached = n(has_pack), n(has_pack & passed)

    sends = tx.filter(passed & (pl.col(elig_col) > 0))
    lab_sends = sends.filter(pl.col("arb_target").is_not_null()).join(hit, on="master_tx_id", how="left").with_columns(pl.col("hit").fill_null(0))
    return {
        "threshold": threshold,
        "k": k,
        "eligibility": mode,
        "positive_tx": pos.height,
        "pack_availability": with_pack / pos.height if pos.height else None,
        "positive_tx_with_covering_pack": with_pack,
        "gate_and_arb_recall_given_pack": reached / with_pack if with_pack else None,
        f"topk_success_given_sent_and_pack": success / reached if reached else None,
        "end_to_end_recall": success / pos.height if pos.height else None,
        "end_to_end_recall_ci": wilson(success, pos.height),
        "succeeded": success,
        "missed": missed,
        "sends_all": sends.height,
        "sends_labelled": lab_sends.height,
        "send_precision_arb": float(lab_sends["arb_target"].mean()) if lab_sends.height else None,
        "send_precision_pack": float(lab_sends["hit"].mean()) if lab_sends.height else None,
        "sends_per_chain_hour": sends.height / hours if hours and hours > 0 else None,
    }
