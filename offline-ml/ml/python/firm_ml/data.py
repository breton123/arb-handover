"""Load an `ml-prep` output directory.

Feature columns come only from `feature_manifest.json`. Anything listed there as
an id, label or meta column can never be selected as a feature.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import polars as pl

TX_KEY = "master_tx_id"


@dataclass(frozen=True)
class Prepared:
    root: Path
    manifest: dict
    feature_manifest: dict

    @property
    def tx_features(self) -> list[str]:
        return [f["name"] for f in self.feature_manifest["tx_features"]]

    @property
    def pack_features(self) -> list[str]:
        return [f["name"] for f in self.feature_manifest["pack_features"]]

    def pack_features_in(self, *groups: str) -> list[str]:
        """Pack features whose group is one of `groups` (`static`, `train_meta`)."""
        return [f["name"] for f in self.feature_manifest["pack_features"] if f["group"] in groups]

    @property
    def kinds(self) -> dict[str, str]:
        fm = self.feature_manifest
        return {f["name"]: f["kind"] for f in fm["tx_features"] + fm["pack_features"]}

    @property
    def gate_columns(self) -> list[str]:
        return list(self.feature_manifest.get("gate_columns", []))

    @property
    def forbidden(self) -> set[str]:
        fm = self.feature_manifest
        return set(fm["id_columns"]) | set(fm["label_columns"]) | set(fm["meta_columns"]) | set(self.gate_columns)


def open_prepared(root: str | Path) -> Prepared:
    root = Path(root)
    return Prepared(
        root=root,
        manifest=json.loads((root / "manifest.json").read_text()),
        feature_manifest=json.loads((root / "feature_manifest.json").read_text()),
    )


def check_same_labels(a: Prepared, b: Prepared) -> None:
    """Two prepared datasets that differ only in pack catalog: same labels, positives and raw source."""
    for name in ("trigger_occurrences", "positives", "route_catalog", "execution_catalog"):
        if a.manifest["inputs"][name]["sha256"] != b.manifest["inputs"][name]["sha256"]:
            raise ValueError(f"{name} differs between {a.root} and {b.root}")
    if a.manifest["raw_source"]["digest"] != b.manifest["raw_source"]["digest"]:
        raise ValueError("raw sources differ")


def check_feature_columns(prep: Prepared, columns: list[str]) -> None:
    """Raise if any column is not a declared feature or is an id/label/meta column."""
    allowed = set(prep.tx_features) | set(prep.pack_features)
    leaked = [c for c in columns if c in prep.forbidden]
    unknown = [c for c in columns if c not in allowed]
    if leaked or unknown:
        raise ValueError(f"not model features: forbidden={leaked} undeclared={unknown}")


def _parts(prep: Prepared, sub: str) -> list[Path]:
    names = prep.manifest["labels"]["parts"]
    return [prep.root / sub / n for n in names]


def load_transactions(prep: Prepared) -> pl.DataFrame:
    """One row per master transaction: ids, labels, meta, relevance gate, then features in manifest order."""
    labels = pl.concat([pl.read_parquet(p) for p in _parts(prep, "tx_labels")])
    feats = pl.concat([pl.read_parquet(p) for p in _parts(prep, "tx_features")])
    if labels.height != feats.height or not (labels[TX_KEY] == feats[TX_KEY]).all():
        raise ValueError("tx_labels and tx_features rows are not aligned")
    feats = feats.select(["block_time", "encode_status", *prep.gate_columns, *prep.tx_features])
    return labels.hstack(feats)


def load_pairs(prep: Prepared) -> pl.DataFrame:
    return pl.concat([pl.read_parquet(p) for p in _parts(prep, "pairs")])


def load_packs(prep: Prepared) -> pl.DataFrame:
    packs = pl.read_parquet(prep.root / "packs" / "pack_features.parquet")
    missing = [c for c in prep.pack_features if c not in packs.columns]
    if missing:
        raise ValueError(f"pack_features.parquet lacks {missing}")
    return packs


def load_programs(prep: Prepared) -> dict[int, str]:
    d = pl.read_parquet(prep.root / "dictionaries" / "programs.parquet")
    return dict(zip(d["program_hash"].to_list(), d["program_id"].to_list()))


def arb_training_rows(tx: pl.DataFrame) -> pl.DataFrame:
    """Rows with a supervised arb target. UNKNOWN (null target) is excluded, never 0."""
    return tx.filter(pl.col("arb_target").is_not_null() & (pl.col("encode_status") == 0))


def ranker_frame(prep: Prepared, tx: pl.DataFrame, pairs: pl.DataFrame, packs: pl.DataFrame) -> pl.DataFrame:
    """Eligible (transaction, pack) rows with tx features, pack features and the split column."""
    tx_cols = [TX_KEY, "split", *prep.tx_features]
    pack_cols = ["pack_id", "builder_rank", "pack_layer", *prep.pack_features]
    out = pairs.drop("split", strict=False).join(tx.select(tx_cols), on=TX_KEY, how="inner").join(packs.select(pack_cols), on="pack_id", how="inner")
    if out.height != pairs.height:
        raise ValueError("pairs lost rows joining tx features or packs")
    return out.sort([TX_KEY, "pack_id"])
