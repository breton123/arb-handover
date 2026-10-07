"""Load a weekly run: one `ml-prep` output per labelled partition plus the frozen candidate library.

Layout (what `weekly/fetch.py` writes, mirroring the Tigris run prefix)::

    <root>/library/   library/{library.json,...}, windows.json, library-train.json, sweep-library-{val,test}.json
    <root>/prep/<partition>/   one ml-prep output per partition

Each partition is prepared on its own with partition-local `master_tx_id`. Loaders re-key every row to the
library's global id `partition_index << 40 | master_tx_id` (column `master_tx_id`; the local id is kept as
`local_master_tx_id`), so joins, splits and the library's train window all use one id space.

`open_week` refuses partitions that disagree on feature schema, pack catalog, library or windows.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import polars as pl

from .data import TX_KEY, Prepared, open_prepared

SHIFT = 40
# Free-text label columns: large at weekly scale and never used by the research loop.
HEAVY_TX_COLUMNS = ("signature", "trigger_keys", "profitable_routes", "unknown_reasons")


@dataclass(frozen=True)
class Week:
    root: Path
    parts: list[Prepared]          # ascending partition_index
    windows: dict                  # the library's global slot / id windows
    library: dict                  # library.json

    @property
    def prep(self) -> Prepared:
        """Representative partition: feature manifest, pack catalog and library provenance are identical across parts."""
        return self.parts[0]

    def index(self, p: Prepared) -> int:
        return int(p.manifest["config"]["partition_index"])

    @property
    def bounds(self) -> dict:
        """Slot boundaries in `splits.slot_boundaries` form, from the library's windows."""
        w = self.windows
        return {"train": [w["train"]["slot_min"], w["train"]["slot_max"]], "val": [w["val"]["slot_min"], w["val"]["slot_max"]],
                "test": [w["test"]["slot_min"], w["test"]["slot_max"]], "fractions": w["fractions"],
                "distinct_slots": w["distinct_slots"]}


def _sha(p: Prepared, path: str) -> str:
    return next(o["sha256"] for o in p.manifest["outputs"] if o["path"] == path)


def open_week(root: str | Path) -> Week:
    root = Path(root)
    parts = [open_prepared(d) for d in sorted((root / "prep").iterdir()) if (d / "manifest.json").exists()]
    if not parts:
        raise ValueError(f"no prepared partitions under {root / 'prep'}")
    parts.sort(key=lambda p: int(p.manifest["config"]["partition_index"]))
    idx = [int(p.manifest["config"]["partition_index"]) for p in parts]
    if len(set(idx)) != len(idx):
        raise ValueError(f"duplicate partition_index: {idx}")
    windows = json.loads((root / "library" / "windows.json").read_text())
    library = json.loads((root / "library" / "library" / "library.json").read_text())
    first = parts[0]
    for p in parts:
        if p.manifest["config"].get("global_id_shift") != SHIFT:
            raise ValueError(f"{p.root}: global_id_shift {p.manifest['config'].get('global_id_shift')} != {SHIFT}")
        for path in ("feature_manifest.json", "packs/pack_features.parquet"):
            if _sha(p, path) != _sha(first, path):
                raise ValueError(f"{p.root}: {path} differs from {first.root}")
        for name in ("windows.json", "library.json", "sweep-library-val.json", "sweep-library-test.json"):
            if p.manifest["provenance"][name]["sha256"] != first.manifest["provenance"][name]["sha256"]:
                raise ValueError(f"{p.root}: provenance {name} differs from {first.root}")
    if json.loads(json.dumps(first.manifest["provenance"]["windows.json"]["content"])) != windows:
        raise ValueError("library windows.json differs from the windows the partitions were prepared against")
    return Week(root=root, parts=parts, windows=windows, library=library)


def _global(index: int) -> pl.Expr:
    return (pl.lit(index << SHIFT, dtype=pl.UInt64) + pl.col(TX_KEY).cast(pl.UInt64))


def _rekey(df: pl.DataFrame, index: int) -> pl.DataFrame:
    return df.with_columns(pl.col(TX_KEY).alias("local_master_tx_id"), _global(index).alias(TX_KEY),
                           pl.lit(index, dtype=pl.UInt16).alias("partition_index"))


def load_transactions(week: Week, heavy: bool = False) -> pl.DataFrame:
    """All partitions' transactions (labels, meta, gate, features in manifest order), re-keyed to global ids."""
    prep = week.prep
    frames = []
    for p in week.parts:
        parts = p.manifest["labels"]["parts"]
        labels = pl.concat([pl.read_parquet(p.root / "tx_labels" / n) for n in parts])
        if not heavy:
            labels = labels.drop([c for c in HEAVY_TX_COLUMNS if c in labels.columns])
        feats = pl.concat([pl.read_parquet(p.root / "tx_features" / n, columns=[TX_KEY, "block_time", "encode_status",
                                                                              *prep.gate_columns, *prep.tx_features])
                           for n in parts])
        if labels.height != feats.height or not (labels[TX_KEY] == feats[TX_KEY]).all():
            raise ValueError(f"{p.root}: tx_labels and tx_features rows are not aligned")
        frames.append(_rekey(labels.hstack(feats.drop(TX_KEY)), week.index(p)))
    tx = pl.concat(frames)
    if tx[TX_KEY].n_unique() != tx.height:
        raise ValueError("global master ids are not unique")
    return tx


def iter_pairs(week: Week, slot_range: tuple[int, int] | None = None) -> Iterator[tuple[Prepared, pl.DataFrame]]:
    """Per partition: its pair rows (re-keyed), optionally restricted to an inclusive slot range."""
    for p in week.parts:
        if slot_range is not None and not (p.manifest["config"]["min_slot"] <= slot_range[1]
                                           and p.manifest["config"]["max_slot"] >= slot_range[0]):
            continue
        df = pl.concat([pl.read_parquet(p.root / "pairs" / n) for n in p.manifest["labels"]["parts"]])
        if slot_range is not None:
            df = df.filter(pl.col("slot").is_between(*slot_range))
        if df.height:
            yield p, _rekey(df, week.index(p))


def load_pairs(week: Week, slot_range: tuple[int, int] | None = None) -> pl.DataFrame:
    return pl.concat([df for _, df in iter_pairs(week, slot_range)])


def summary(week: Week) -> dict:
    """Totals across partitions for the notebook overview."""
    ms = [p.manifest for p in week.parts]
    return {
        "partitions": len(ms),
        "partition_indexes": [int(m["config"]["partition_index"]) for m in ms],
        "dataset_ids": [m["config"]["dataset_id"] for m in ms],
        "labels": {k: sum(m["labels"][k] for m in ms) for k in ("tx_positive", "tx_certified_negative", "tx_unknown")},
        "pairs": {k: sum(m["pairs"][k] for m in ms) for k in ("rows", "positive_target", "transactions")},
        "v1_format_txs": sum(m["features"].get("v1_format") or 0 for m in ms),
        "manifest_sha256": {m["config"]["dataset_id"]: _sha_file(p.root / "manifest.json") for m, p in zip(ms, week.parts)},
    }


def _sha_file(path: Path) -> str:
    from .models import sha256_file
    return sha256_file(path)
