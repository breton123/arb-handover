#!/usr/bin/env python3
"""Golden hop fixtures: semantic flatten must match stored keys.

No RPC. Catches the mx/my wipe class of regression: if a future
universe/ALT/compiler change rebuilds a different semantic vector,
the fixture compare fails before we lose Custom(6) silently.
"""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPTS = HERE.parent / "scripts"
sys.path.insert(0, str(SCRIPTS))

import hops_vector as hv  # noqa: E402

GOLDEN = HERE / "golden_hops"


class TestGoldenHops(unittest.TestCase):
    def test_flatten_lengths(self):
        buy = hv.pump_roles(True)
        sell = hv.pump_roles(False)
        self.assertEqual(len(buy), 26)
        self.assertEqual(len(sell), 24)
        self.assertEqual(buy[23], "pool_v2")
        self.assertEqual(buy[24], "fee_recipient")
        self.assertEqual(buy[25], "fee_recipient_quote_ata")
        self.assertEqual(sell[21], "pool_v2")
        self.assertEqual(sell[22], "fee_recipient")
        self.assertEqual(sell[23], "fee_recipient_quote_ata")
        self.assertNotIn("global_volume_accumulator", sell)
        self.assertEqual(len(hv.DLMM_ROLES), 10)

    def test_anchor_2006_parse(self):
        logs = [
            "Program pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA invoke [2]",
            "Program log: AnchorError caused by account: creator_vault. "
            "Error Code: ConstraintSeeds. Error Number: 2006. "
            "Error Message: A seeds constraint was violated.",
            "Program log: Left:",
            "Program log: ActualPubkey111111111111111111111111111111",
            "Program log: Right:",
            "Program log: ExpectedPda1111111111111111111111111111111",
        ]
        p = hv.parse_anchor_constraint(logs)
        self.assertEqual(p["caused_by"], "creator_vault")
        self.assertEqual(p["error_number"], 2006)
        self.assertIn("ActualPubkey", p["actual"])
        self.assertIn("ExpectedPda", p["expected"])
        self.assertEqual(p["invokes"], ["pump"])

    def test_anchor_3005_hex(self):
        logs = [
            "Program LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo invoke [2]",
            "Program log: custom program error: 0xbbd",
        ]
        p = hv.parse_anchor_constraint(logs)
        self.assertEqual(p["error_number"], 3005)
        self.assertEqual(p["invokes"], ["dlmm"])

    def test_frozen_fixtures_match_semantic(self):
        if not GOLDEN.is_dir():
            self.skipTest("no golden_hops dir")
        found = list(GOLDEN.glob("*.json"))
        if not found:
            self.skipTest("no fixtures yet")
        for path in found:
            with self.subTest(path.name):
                doc = json.loads(path.read_text(encoding="utf-8"))
                rebuilt = hv.flatten_fixture(doc)
                self.assertEqual(rebuilt, doc["keys"], path.name)
                self.assertTrue(doc.get("custom6"), path.name)
                self.assertEqual(doc.get("seq"), _seq_from_name(path.stem))
                self.assertEqual(len(doc.get("hops") or []), doc["seq"].count("-") + 1)


def _seq_from_name(stem: str) -> str:
    return {
        "dd": "dlmm-dlmm",
        "dp": "dlmm-pump",
        "pd": "pump-dlmm",
        "pp": "pump-pump",
        "ddd": "dlmm-dlmm-dlmm",
        "ddp": "dlmm-dlmm-pump",
        "pdd": "pump-dlmm-dlmm",
    }[stem]


if __name__ == "__main__":
    raise SystemExit(unittest.main())
