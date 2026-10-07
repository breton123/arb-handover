import numpy as np
import polars as pl

from firm_ml import encoding, models

KINDS = {"h": "hash", "x": "numeric"}


def frame(n=400, seed=0):
    rng = np.random.default_rng(seed)
    counts = {2**63 + 5: n // 2, 17: n * 3 // 10, 99: 2}  # 99 is rare (2 < min_count)
    counts[2**40 + 3] = n - sum(counts.values())
    h = rng.permutation(np.repeat(np.array(list(counts), dtype=np.uint64), list(counts.values())))
    x = rng.integers(0, 100, n).astype(np.uint32)
    y = ((x > 60) | (h == 17)).astype(int)
    return pl.DataFrame({"h": h, "x": x, "y": y})


def test_hash_vocab_is_deterministic_and_unseen_is_missing(tmp_path):
    df = frame()
    a = encoding.FeatureEncoder(["h", "x"], KINDS, min_count=5).fit(df)
    b = encoding.FeatureEncoder(["h", "x"], KINDS, min_count=5).fit(df.reverse())
    assert a.vocab == b.vocab
    assert a.vocab["h"][2**63 + 5] == 0 and 99 not in a.vocab["h"]  # rare hash stays out of the vocabulary
    probe = pl.DataFrame({"h": np.array([2**63 + 5, 123], dtype=np.uint64), "x": np.array([7, 8], dtype=np.uint32)})
    X = a.transform(probe)
    assert X[0, 0] == 0 and np.isnan(X[1, 0]) and list(X[:, 1]) == [7, 8]
    a.save(tmp_path / "enc.json")
    c = encoding.FeatureEncoder.load(tmp_path / "enc.json")
    assert c.vocab == a.vocab and c.columns == a.columns
    assert np.array_equal(c.transform(df), a.transform(df), equal_nan=True)


def test_model_roundtrip_keeps_feature_order_and_scores(tmp_path):
    tr, va = frame(seed=1), frame(seed=2)
    enc = encoding.FeatureEncoder(["x", "h"], KINDS).fit(tr)
    params = {**models.ARB_PARAMS, "num_threads": 1}
    bst = models.train_binary(enc.transform(tr), tr["y"].to_numpy(), enc.transform(va), va["y"].to_numpy(),
                              enc, params, rounds=50, early_stopping=10)
    models.save_model(bst, enc, tmp_path, "m", params, {})
    b2, e2, meta = models.load_model(tmp_path, "m")
    assert meta["feature_order"] == ["x", "h"] == b2.feature_name()
    assert np.allclose(b2.predict(e2.transform(va)), bst.predict(enc.transform(va), num_iteration=bst.best_iteration))
