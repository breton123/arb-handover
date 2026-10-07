"""LightGBM training and persistence.

A saved model is `<name>.txt` (LightGBM text format, readable by the C API /
Rust bindings) plus `<name>.json` with the exact feature order, the encoder,
the parameters and the training summary.
"""

from __future__ import annotations

import hashlib
import json
import platform
from pathlib import Path

import lightgbm as lgb
import numpy as np

from .encoding import FeatureEncoder

ARB_PARAMS = {
    "objective": "binary",
    "learning_rate": 0.03,
    "num_leaves": 15,
    "min_data_in_leaf": 20,
    "feature_fraction": 0.8,
    "bagging_fraction": 0.8,
    "bagging_freq": 1,
    "lambda_l2": 1.0,
    "max_cat_to_onehot": 8,
    "cat_smooth": 10.0,
    "min_data_per_group": 20,
    "metric": ["average_precision", "binary_logloss"],
    "seed": 7,
    "deterministic": True,
    "force_row_wise": True,
    "num_threads": 8,
    "verbose": -1,
}

RANKER_PARAMS = {**ARB_PARAMS, "num_leaves": 15, "min_data_in_leaf": 10}


def train_binary(
    X_tr: np.ndarray,
    y_tr: np.ndarray,
    X_va: np.ndarray,
    y_va: np.ndarray,
    encoder: FeatureEncoder,
    params: dict,
    rounds: int = 2000,
    early_stopping: int = 100,
) -> lgb.Booster:
    """Unweighted binary objective, early-stopped on validation PR-AUC.

    No class reweighting: the score stays a probability estimate on the
    training base rate, so thresholds and calibration curves mean something.
    """
    cat = [encoder.columns.index(c) for c in encoder.categorical]
    dtr = lgb.Dataset(X_tr, y_tr, feature_name=encoder.columns, categorical_feature=cat, free_raw_data=False)
    dva = lgb.Dataset(X_va, y_va, reference=dtr, categorical_feature=cat, free_raw_data=False)
    return lgb.train(
        params,
        dtr,
        num_boost_round=rounds,
        valid_sets=[dva],
        valid_names=["val"],
        callbacks=[lgb.early_stopping(early_stopping, first_metric_only=True, verbose=False)],
    )


def importance(booster: lgb.Booster, kind: str = "gain") -> list[tuple[str, float]]:
    vals = booster.feature_importance(importance_type=kind)
    return sorted(zip(booster.feature_name(), map(float, vals)), key=lambda x: -x[1])


def save_model(booster: lgb.Booster, encoder: FeatureEncoder, out_dir: Path, name: str, params: dict, extra: dict) -> dict:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    model_path = out_dir / f"{name}.txt"
    booster.save_model(model_path)
    enc_path = out_dir / f"{name}.encoder.json"
    encoder.save(enc_path)
    sidecar = {
        "name": name,
        "model_file": model_path.name,
        "model_sha256": sha256_file(model_path),
        "encoder_file": enc_path.name,
        "encoder_sha256": sha256_file(enc_path),
        "feature_order": encoder.columns,
        "categorical_features": encoder.categorical,
        "best_iteration": booster.best_iteration,
        "params": params,
        **extra,
    }
    (out_dir / f"{name}.json").write_text(json.dumps(sidecar, indent=1, default=str))
    return sidecar


def load_model(out_dir: Path, name: str) -> tuple[lgb.Booster, FeatureEncoder, dict]:
    out_dir = Path(out_dir)
    meta = json.loads((out_dir / f"{name}.json").read_text())
    booster = lgb.Booster(model_file=str(out_dir / meta["model_file"]))
    encoder = FeatureEncoder.load(out_dir / meta["encoder_file"])
    if booster.feature_name() != meta["feature_order"] or encoder.columns != meta["feature_order"]:
        raise ValueError(f"{name}: saved feature order disagrees with model")
    return booster, encoder, meta


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def environment() -> dict:
    import polars, sklearn, pyarrow
    return {
        "python": platform.python_version(),
        "lightgbm": lgb.__version__,
        "numpy": np.__version__,
        "polars": polars.__version__,
        "pyarrow": pyarrow.__version__,
        "scikit-learn": sklearn.__version__,
        "platform": platform.platform(),
    }
