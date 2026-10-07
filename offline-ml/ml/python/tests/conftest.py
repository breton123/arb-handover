import os
from pathlib import Path

import pytest

from firm_ml import data

REPO = Path(__file__).resolve().parents[3]
PREPARED = Path(os.environ.get("FIRM_ML_PREPARED", "D:/firm-ml/prepared/ml-prep-054-003"))
DEV = Path(os.environ.get("FIRM_ML_DEV", "D:/firm-ml/dev-100k"))


@pytest.fixture(scope="session")
def prep():
    if not (PREPARED / "manifest.json").exists():
        pytest.skip(f"no prepared dataset at {PREPARED} (set FIRM_ML_PREPARED)")
    return data.open_prepared(PREPARED)


@pytest.fixture(scope="session")
def tx(prep):
    return data.load_transactions(prep)
