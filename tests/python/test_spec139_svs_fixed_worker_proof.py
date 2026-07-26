#!/usr/bin/env python3
"""Contract tests for Spec 139 fixed-rate proof."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
RUNNER = REPO / "Experiments/NDN_SVS_Fixed_Worker_Proof_Minindn.py"
ANALYZER = REPO / "Experiments/analyze_svs_fixed_worker_proof.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec139ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.runner = load_module("spec139_runner", RUNNER)
        cls.analyzer = load_module("spec139_analyzer", ANALYZER)

    def test_fixed_rate_is_600_and_not_operator_selectable(self):
        self.assertEqual(self.runner.RATE, 600)
        parser_source = RUNNER.read_text(encoding="utf-8")
        self.assertNotIn("--rate", parser_source)

    def test_same_subject_binary_and_exact_two_modes(self):
        subject = self.runner.prior.verify_subject()
        self.assertEqual(subject["binarySha256"], self.runner.prior.EXPECTED_BINARY)
        configs = self.runner.prior.base.self_test_configs(subject)
        self.assertEqual(set(configs), {"face-serial", "worker-serial"})
        self.assertEqual(configs["worker-serial"]["production_workers"], 1)
        self.assertEqual(configs["worker-serial"]["receive_workers"], 0)

    def test_formal_matrix_is_exact_and_once_only(self):
        cells = self.runner.formal_cells()
        self.assertEqual(len(cells), 6)
        self.assertEqual(
            [(row["ordinal"], row["pair"], row["mode"]) for row in cells],
            [
                (1, 1, "face-serial"), (2, 1, "worker-serial"),
                (3, 2, "worker-serial"), (4, 2, "face-serial"),
                (5, 3, "face-serial"), (6, 3, "worker-serial"),
            ],
        )
        self.assertTrue(all(row["rate"] == 600 for row in cells))
        self.assertTrue(
            all(
                (row["warmup"], row["measure"], row["drain"]) == (10, 60, 10)
                for row in cells
            )
        )

    def test_receipt_ledger_rejects_second_write(self):
        with tempfile.TemporaryDirectory() as directory:
            ledger = self.runner.ReceiptLedger(Path(directory))
            receipt = {
                "schema": "spec139.receipt.v1",
                "ordinal": 1,
                "retryCount": 0,
            }
            ledger.append(receipt)
            with self.assertRaisesRegex(RuntimeError, "already exists"):
                ledger.append(receipt)

    def test_qualification_requires_both_modes_and_face_pressure(self):
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('"face_admissible"', source)
        self.assertIn('"worker_admissible"', source)
        self.assertIn('"face_pressure"', source)
        self.assertIn("all(checks.values())", source)

    def test_analyzer_uses_registered_run_level_classifier(self):
        source = ANALYZER.read_text(encoding="utf-8")
        self.assertIn("base.paired_contrast", source)
        self.assertIn("base.classify", source)
        self.assertIn("expected six receipts", source)


if __name__ == "__main__":
    unittest.main()
