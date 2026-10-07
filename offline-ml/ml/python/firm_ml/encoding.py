"""Model-matrix encoding that Rust inference can reproduce.

- `numeric`: the stored integer as float64.
- `categorical`: the stored small code, passed to LightGBM as categorical.
- `hash`: 64-bit hash -> train-fitted vocabulary code (0..k-1, by descending
  train frequency, ties by hash value). Unseen or rare hashes -> missing
  (NaN), which LightGBM routes like any missing categorical value.

The encoder is saved as JSON. Hash keys are decimal strings because they
exceed 2**53.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import polars as pl

UNSEEN = None  # missing


@dataclass
class FeatureEncoder:
    columns: list[str]
    kinds: dict[str, str]
    vocab: dict[str, dict[int, int]] = field(default_factory=dict)
    max_vocab: int = 255
    min_count: int = 5

    @property
    def categorical(self) -> list[str]:
        return [c for c in self.columns if self.kinds[c] in ("hash", "categorical")]

    def fit(self, train: pl.DataFrame) -> "FeatureEncoder":
        for c in self.columns:
            if self.kinds[c] != "hash":
                continue
            counts = (
                train.group_by(c).len()
                .filter(pl.col("len") >= self.min_count)
                .sort(["len", c], descending=[True, False])
                .head(self.max_vocab)
            )
            self.vocab[c] = {int(h): i for i, h in enumerate(counts[c].to_list())}
        return self

    def transform(self, df: pl.DataFrame) -> np.ndarray:
        out = np.empty((df.height, len(self.columns)), dtype=np.float64)
        for j, c in enumerate(self.columns):
            if self.kinds[c] == "hash":
                mapping = self.vocab[c]
                codes = df[c].replace_strict(mapping, default=None, return_dtype=pl.Float64)
                out[:, j] = codes.fill_null(float("nan")).to_numpy()
            else:
                out[:, j] = df[c].cast(pl.Float64).to_numpy()
        return out

    def to_json(self) -> dict:
        return {
            "columns": self.columns,
            "kinds": {c: self.kinds[c] for c in self.columns},
            "categorical": self.categorical,
            "unseen": "missing (NaN)",
            "max_vocab": self.max_vocab,
            "min_count": self.min_count,
            "vocab": {c: {str(h): i for h, i in v.items()} for c, v in self.vocab.items()},
        }

    def save(self, path: Path) -> None:
        Path(path).write_text(json.dumps(self.to_json(), indent=1))

    @classmethod
    def load(cls, path: Path) -> "FeatureEncoder":
        d = json.loads(Path(path).read_text())
        enc = cls(columns=d["columns"], kinds=d["kinds"], max_vocab=d["max_vocab"], min_count=d["min_count"])
        enc.vocab = {c: {int(h): i for h, i in v.items()} for c, v in d["vocab"].items()}
        return enc
