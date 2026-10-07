"""ml-prep header features equal the existing `tx-features encode-jsonl` output on the same raw bytes.

Uses the dev build over raw-tx-100k.jsonl:
    ml-prep build --raw-jsonl raw-tx-100k.jsonl --max-slot 446457297 --out D:/firm-ml/dev-100k ...
"""

import subprocess

import polars as pl
import pytest

from conftest import DEV, REPO
from firm_ml import data


def test_header_features_match_existing_encoder_cli(tmp_path):
    if not (DEV / "manifest.json").exists():
        pytest.skip(f"no dev build at {DEV} (set FIRM_ML_DEV)")
    cli = REPO / "target" / "release" / "tx-features.exe"
    if not cli.exists():
        cli = cli.with_suffix("")
    if not cli.exists():
        subprocess.run(["cargo", "build", "--release", "-p", "tx-features-cli"], cwd=REPO, check=True)
        cli = REPO / "target" / "release" / "tx-features.exe"
        if not cli.exists():
            cli = cli.with_suffix("")
    out = tmp_path / "cli.parquet"
    subprocess.run([str(cli), "encode-jsonl", "--input", str(REPO / "raw-tx-100k.jsonl"), "--output", str(out)], check=True)

    prep = data.open_prepared(DEV)
    tx = data.load_transactions(prep)
    cli_df = pl.read_parquet(out).with_columns(pl.col("tx_index").cast(pl.UInt64))
    header = [f["name"] for f in prep.feature_manifest["tx_features"] if f["name"] in cli_df.columns]
    assert len(header) >= 45
    j = tx.select("slot", "tx_index", "signature", *header).join(
        cli_df.select("slot", "tx_index", "signature", *header), on=["slot", "tx_index"], suffix="_cli")
    assert j.height == tx.height
    assert (j["signature"] == j["signature_cli"]).all()
    for c in header:
        assert (j[c].cast(pl.UInt64) == j[f"{c}_cli"].cast(pl.UInt64)).all(), c
