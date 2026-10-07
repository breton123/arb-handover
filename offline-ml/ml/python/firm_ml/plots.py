"""Notebook figures. Static matplotlib; every figure is also saved as PNG.

At most three series per chart (blue, orange, aqua: the first three slots of
the validated categorical palette), so no pair is confusable under CVD.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from sklearn.metrics import precision_recall_curve

SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
SURFACE = "#fcfcfb"


def style() -> None:
    plt.rcParams.update({
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "axes.edgecolor": GRID,
        "axes.labelcolor": INK_2,
        "axes.titlecolor": INK,
        "axes.titlesize": 11,
        "axes.titleweight": "bold",
        "axes.grid": True,
        "axes.axisbelow": True,
        "grid.color": GRID,
        "grid.linewidth": 0.8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "xtick.color": INK_2,
        "ytick.color": INK_2,
        "legend.frameon": False,
        "lines.linewidth": 2,
        "font.size": 9,
    })


def _save(fig, path: Path | None):
    fig.tight_layout()
    if path is not None:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(path, dpi=130)
    return fig


def pr_curves(curves: dict[str, tuple[np.ndarray, np.ndarray]], title: str, path: Path | None = None):
    """curves: name -> (y, p). Base rate drawn as a dashed reference per series."""
    fig, ax = plt.subplots(figsize=(5.5, 4))
    for (name, (y, p)), color in zip(curves.items(), SERIES):
        prec, rec, _ = precision_recall_curve(y, p)
        ax.plot(rec, prec, color=color, label=f"{name} (n+={int(y.sum())})")
        ax.axhline(y.mean(), color=color, linewidth=1, linestyle=":")
    ax.set(xlabel="Recall (positive transactions)", ylabel="Precision", xlim=(0, 1), ylim=(0, 1.02), title=title)
    ax.legend(loc="upper right")
    return _save(fig, path)


def calibration(tables: dict, title: str, path: Path | None = None):
    """tables: name -> polars frame with mean_pred, observed, n (from metrics.calibration_table)."""
    fig, ax = plt.subplots(figsize=(5, 4))
    ax.plot([0, 1], [0, 1], color=INK_2, linewidth=1, linestyle="--", label="perfect")
    for (name, t), color in zip(tables.items(), SERIES):
        ax.plot(t["mean_pred"], t["observed"], color=color, marker="o", markersize=5, label=name)
    ax.set(xlabel="Mean predicted probability (quantile bin)", ylabel="Observed positive rate", title=title,
           xlim=(0, 1), ylim=(0, 1))
    ax.legend(loc="upper left")
    return _save(fig, path)


def score_hist(scores: dict[str, np.ndarray], title: str, path: Path | None = None, bins: int = 40):
    fig, ax = plt.subplots(figsize=(5.5, 3.6))
    edges = np.linspace(0, 1, bins + 1)
    for (name, s), color in zip(scores.items(), SERIES):
        ax.hist(s, bins=edges, density=True, histtype="step", color=color, linewidth=2, label=f"{name} (n={len(s)})")
    ax.set(xlabel="Arb probability", ylabel="Density", title=title, yscale="log")
    ax.legend()
    return _save(fig, path)


def feature_by_label(values: dict[str, np.ndarray], feature: str, path: Path | None = None):
    fig, ax = plt.subplots(figsize=(4.2, 3))
    allv = np.concatenate([v for v in values.values() if len(v)])
    lo, hi = np.percentile(allv, [0.5, 99.5]) if len(allv) else (0, 1)
    edges = np.linspace(lo, hi if hi > lo else lo + 1, 31)
    for (name, v), color in zip(values.items(), SERIES):
        ax.hist(np.clip(v, edges[0], edges[-1]), bins=edges, density=True, histtype="step", color=color, linewidth=2, label=name)
    ax.set(title=feature, ylabel="Density")
    ax.legend(fontsize=8)
    return _save(fig, path)


def importance(pairs: list[tuple[str, float]], title: str, path: Path | None = None, top: int = 20):
    pairs = pairs[:top][::-1]
    fig, ax = plt.subplots(figsize=(6, 0.28 * len(pairs) + 1))
    ax.barh([p[0] for p in pairs], [p[1] for p in pairs], color=SERIES[0], height=0.6)
    ax.set(xlabel="Total gain", title=title)
    ax.grid(axis="y", visible=False)
    return _save(fig, path)


def miss_breakdown(rows: list[dict], title: str, path: Path | None = None):
    """rows: [{label, succeeded, ranker, classifier, relevance_gate, no_candidate_pack}] -> stacked bars."""
    parts = [("succeeded", SERIES[2], "white"), ("ranker", SERIES[1], "white"), ("classifier", SERIES[0], "white"),
             ("relevance_gate", "#eda100", INK), ("no_candidate_pack", INK_2, "white"),
             ("encoder_unsupported", "#b9b7b0", INK)]
    fig, ax = plt.subplots(figsize=(7.5, 0.5 * len(rows) + 1.5))
    labels = [r["label"] for r in rows]
    left = np.zeros(len(rows))
    for key, color, text in parts:
        vals = np.array([r.get(key, 0) for r in rows], dtype=float)
        ax.barh(labels, vals, left=left, color=color, height=0.6, edgecolor=SURFACE, linewidth=2,
                label=key.replace("_", " "))
        for i, v in enumerate(vals):
            if v > 0:
                ax.text(left[i] + v / 2, i, f"{int(v)}", ha="center", va="center", color=text, fontsize=8)
        left += vals
    ax.set(xlabel="Positive transactions", title=title)
    ax.grid(axis="y", visible=False)
    ax.invert_yaxis()
    ax.legend(ncol=5, loc="upper center", bbox_to_anchor=(0.5, -0.3 if len(rows) < 4 else -0.14), fontsize=8)
    return _save(fig, path)


def lines(series: dict[str, tuple[list, list]], title: str, xlabel: str, ylabel: str, path: Path | None = None,
          logx: bool = False):
    """Up to three named series, each (x, y), drawn with markers and a direct end label."""
    fig, ax = plt.subplots(figsize=(6, 4))
    for (name, (x, y)), color in zip(series.items(), SERIES):
        pts = [(a, b) for a, b in zip(x, y) if a is not None and b is not None and np.isfinite(a) and a > (0 if logx else -np.inf)]
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax.plot(xs, ys, color=color, marker="o", markersize=5, label=name)
        ax.annotate(name, (xs[-1], ys[-1]), xytext=(4, 0), textcoords="offset points", color=INK_2, fontsize=8, va="center")
    if logx:
        ax.set_xscale("log")
    ax.set(title=title, xlabel=xlabel, ylabel=ylabel)
    ax.set_ylim(bottom=0)
    ax.legend(loc="best")
    return _save(fig, path)
