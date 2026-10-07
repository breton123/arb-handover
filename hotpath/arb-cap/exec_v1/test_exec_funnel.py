#!/usr/bin/env python3
"""Local checks for exec gates. No RPC, no FUNDED, no racer."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-exec" / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import exec_funnel  # noqa: E402
import exec_gates as gates  # noqa: E402
import template_audit as audit  # noqa: E402

POOL_HEX = "00" * 31 + "01"
POOL_B58 = gates.pool_pubkey(POOL_HEX)


def opp(**kw):
    base = {
        "kind": "opp_synced",
        "pool": POOL_HEX,
        "race_ready": 1,
        "frame": {"class": "framed"},
        "arb": {"direction": 0, "amount_in": 1, "gross": 1},
        "send_quote": {"cap_ok": 1, "cap_gross": gates.HURDLE + 1},
        "n": {"pool_idx": 3},
        "fresh": {"shred_slot": 9},
    }
    base.update(kw)
    return base


def plane_ready():
    return gates.index_plane([{
        "dlmm": POOL_B58,
        "pump": "pump",
        "RACE_READY": 1,
        "tmpl0": "aa",
        "tmpl1": "bb",
        "race_ready": 0,
    }])


class RaceFlags(unittest.TestCase):
    def test_and_not_or(self):
        self.assertEqual(gates.race_flags(1, 0)["both"], 0)
        self.assertEqual(gates.race_flags(0, 1)["both"], 0)
        self.assertEqual(gates.race_flags(1, 1)["both"], 1)
        self.assertEqual(gates.race_flags(1, 0)["tx_exact"], 1)
        self.assertEqual(gates.race_flags(1, 0)["plane_race_ready"], 0)

    def test_ix_only_is_not_plane_ready(self):
        rec = opp(race_ready=0)
        self.assertEqual(
            gates.classify_opp(rec, plane_ready(), True),
            "ix_only",
        )

    def test_tx_exact_without_plane_flag(self):
        indexed = gates.index_plane([{
            "dlmm": POOL_B58,
            "pump": "pump",
            "RACE_READY": 0,
            "race_ready": 1,
            "tmpl0": "aa",
            "tmpl1": "bb",
        }])
        self.assertEqual(
            gates.classify_opp(opp(), indexed, True),
            "plane_not_race_ready",
        )

    def test_would_fire_needs_both(self):
        self.assertEqual(gates.classify_opp(opp(), plane_ready(), True), "would_fire")
        self.assertIsNotNone(gates.size_gate(opp(), plane_ready(), True))

    def test_state007_file_does_not_count(self):
        self.assertEqual(
            gates.classify_opp(opp(), plane_ready(), False),
            "ready_file_missing",
        )

    def test_hurdle_is_oneshot_flat(self):
        rec = opp()
        rec["send_quote"] = {"cap_ok": 1, "cap_gross": gates.HURDLE}
        self.assertEqual(
            gates.classify_opp(rec, plane_ready(), True),
            "below_oneshot_hurdle",
        )
        self.assertEqual(gates.HURDLE, 525_000)

    def test_family_255_and_3hop(self):
        self.assertFalse(gates.v1_executable(255, 2))
        self.assertFalse(gates.v1_executable(0, 3))
        self.assertTrue(gates.v1_executable(0, 2))
        self.assertTrue(gates.v1_executable(5, 2))
        self.assertFalse(gates.v1_executable(6, 2))
        self.assertFalse(gates.v1_executable(0, 2, "dlmm-dlmm"))
        self.assertTrue(gates.v1_executable(0, 2, "dlmm-pump"))
        self.assertEqual(gates.wire_hurdle(), 525_000)
        path, err = gates.resolve_auth_ready(None)
        self.assertEqual(path, gates.AUTH_READY_DEFAULT)
        self.assertIsNone(err)
        cap, cap_err = gates.current_auth_cap({"STATE008_CAP": "/data/bsc/captures/soak_pump016_20260926/state008"})
        self.assertIsNone(cap_err)
        self.assertTrue(cap.endswith("soak_pump016_20260926/state008"))
        self.assertEqual(
            str(gates.current_auth_ready({"STATE008_CAP": cap})).replace("\\", "/"),
            cap + "/READY",
        )
        _, cap_err = gates.current_auth_cap({"STATE008_CAP": "/home/louis/captures/state007"})
        self.assertEqual(cap_err, "state007_ready_cannot_arm")
        _, err = gates.resolve_auth_ready(gates.STATE007_READY_ALIAS)
        self.assertEqual(err, "state007_ready_cannot_arm")
        _, err = gates.resolve_auth_ready("/tmp/other/READY")
        self.assertEqual(err, "auth_ready_path_not_mandatory")

    def test_hex_pool_joins_base58_plane(self):
        self.assertEqual(len(POOL_B58) >= 32, True)
        self.assertIsNotNone(gates.plane_row(plane_ready(), POOL_HEX))


class TemplateParse(unittest.TestCase):
    def test_compiled_templates_match_the_lock(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-exec" / "scripts"))
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-cap"))
        import exec_live002b as exe

        static = [
            exe.SYSTEM, audit.OUR_EXEC, exe.CU_PROG, exe.TOKENKEG, exe.SYSTEM,
            exe.ATA_PROG, exe.MEMO, exe.DLMM, exe.PUMP, exe.TOKEN2022, exe.FEE_PROG,
        ]
        sell = audit.parse_template(exe.compile_v0(static, exe.ALT_PROG))
        buy = audit.parse_template(exe.compile_v0_buy(static, exe.ALT_PROG))
        self.assertIsNone(audit.template_problems(sell, False))
        self.assertIsNone(audit.template_problems(buy, True))
        self.assertEqual(sell["disc"], b"ARBEXEC0")
        self.assertEqual(sell["static"][1], audit.OUR_EXEC)
        self.assertEqual(sell["cu_limit"], 400_000)
        self.assertLessEqual(sell["len"], 1232)
        self.assertLessEqual(buy["len"], 1232)


class FunnelFile(unittest.TestCase):
    def test_replay_assigns_one_bucket(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            audit = root / "audit.jsonl"
            univ = root / "univ.json"
            plane = root / "plane.json"
            ready = root / "READY"
            out = root / "funnel.jsonl"
            ready.write_text("1\n", encoding="utf-8")
            univ.write_text(json.dumps({"pools": [{"idx": 3, "pubkey": POOL_B58}]}), encoding="utf-8")
            plane.write_text(json.dumps({"routes": [{
                "dlmm": POOL_B58,
                "pump": "pump",
                "RACE_READY": 1,
                "tmpl0": "aa",
                "tmpl1": "bb",
            }]}), encoding="utf-8")
            rows = [
                {"kind": "gate", "would_send_new": 1, "family": 255, "n_hop": 3,
                 "pool_idx": 3, "shred_slot": 9, "cap_hurdle": 1, "exec_fam": 0},
                {"kind": "gate", "would_send_new": 1, "family": 0, "n_hop": 2,
                 "pool_idx": 3, "shred_slot": 9, "cap_hurdle": 1, "cap_gross": 900000,
                 "exec_fam": 1, "seq": "dlmm-pump"},
                {"kind": "gate", "would_send_new": 1, "family": 0, "n_hop": 2,
                 "pool_idx": 4, "shred_slot": 1, "exec_fam": 1},
                opp(),
                opp(race_ready=0, fresh={"shred_slot": 8}),
            ]
            audit.write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
            rc = exec_funnel.main([
                "--audit", str(audit),
                "--univ", str(univ),
                "--plane", str(plane),
                "--plane-report", str(root / "missing.json"),
                "--ready", str(ready),
                "--cooldown", str(root / "cool.json"),
                "--out", str(out),
            ])
            self.assertEqual(rc, 0)
            got = [json.loads(line) for line in out.read_text(encoding="utf-8").splitlines()]
            wsn = [r["bucket"] for r in got if r["source"] == "would_send_new"]
            self.assertEqual(wsn, ["not_v1_executable", "would_fire", "no_opp_synced"])
            opp_buckets = [r["bucket"] for r in got if r["source"] == "opp_synced"]
            self.assertEqual(opp_buckets.count("would_fire"), 1)
            self.assertIn("ix_only", opp_buckets)
            fired = next(r for r in got if r["bucket"] == "would_fire" and r["source"] == "would_send_new")
            self.assertEqual(fired["tx_exact"], 1)
            self.assertEqual(fired["plane_race_ready"], 1)
            self.assertEqual(fired["race_both"], 1)
            self.assertEqual(fired["send_hurdle"], 525000)


class AmbiguousPlane(unittest.TestCase):
    def test_shared_dlmm_does_not_send(self):
        indexed = gates.index_plane([
            {"dlmm": POOL_B58, "pump": "pumpA", "RACE_READY": 1, "tmpl0": "aa", "tmpl1": "bb"},
            {"dlmm": POOL_B58, "pump": "pumpB", "RACE_READY": 1, "tmpl0": "cc", "tmpl1": "dd"},
        ])
        self.assertEqual(indexed[POOL_B58]["plane_race_ready"], 0)
        self.assertEqual(indexed[POOL_B58]["ambiguous"], 1)
        self.assertEqual(indexed["pumpA"]["plane_race_ready"], 1)
        self.assertEqual(indexed["pumpB"]["plane_race_ready"], 1)
        self.assertEqual(gates.classify_opp(opp(), indexed, True), "ambiguous_pool")
        self.assertIsNone(gates.size_gate(opp(), indexed, True))

    def test_seq_on_the_row_blocks_dlmm_dlmm(self):
        rec = opp()
        rec["arb"] = dict(rec["arb"], seq="dlmm-dlmm", family=6)
        self.assertIsNone(gates.size_gate(rec, plane_ready(), True))


if __name__ == "__main__":
    unittest.main()
