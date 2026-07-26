#!/usr/bin/env python3
"""Contract tests for Spec 138 same-binary necessity confirmation."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
RUNNER = REPO / "Experiments/NDN_SVS_Worker_Necessity_Minindn.py"
ANALYZER = REPO / "Experiments/analyze_svs_worker_necessity.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec138ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.runner = load_module("spec138_runner", RUNNER)
        cls.analysis = load_module("spec138_analysis", ANALYZER)

    def test_one_subject_one_binary_and_two_modes_only(self):
        subject = self.runner.verify_subject()
        self.assertEqual(subject["baseCommit"], self.runner.EXPECTED_COMMIT)
        self.assertEqual(subject["binarySha256"], self.runner.EXPECTED_BINARY)
        configs = self.runner.base.self_test_configs(subject)
        self.assertEqual(set(configs), {"face-serial", "worker-serial"})
        delta = self.runner.base.analysis.runtime_config_delta(
            configs["face-serial"], configs["worker-serial"]
        )
        self.assertEqual(
            delta, self.runner.base.analysis.ALLOWED_TREATMENT_FIELDS
        )
        self.assertEqual(configs["worker-serial"]["production_workers"], 1)
        self.assertEqual(configs["face-serial"]["production_workers"], 0)
        self.assertEqual(configs["worker-serial"]["receive_workers"], 0)

    def test_control_ladder_is_descending_and_control_only(self):
        self.assertEqual(self.runner.RATES, (1000, 800, 600, 400, 200))
        rows = [
            {
                "rate": 1000,
                "mode": "face-serial",
                "admissible": False,
                "pressureGate": True,
            },
            {
                "rate": 800,
                "mode": "face-serial",
                "admissible": True,
                "pressureGate": True,
            },
        ]
        selected = self.analysis.select_control_rate(rows)
        self.assertEqual(selected["selectedRate"], 800)
        self.assertTrue(selected["controlOnly"])

    def test_selector_rejects_reordered_or_unregistered_rows(self):
        rows = [
            {
                "rate": 800,
                "mode": "face-serial",
                "admissible": True,
                "pressureGate": True,
            }
        ]
        with self.assertRaisesRegex(RuntimeError, "registered ladder"):
            self.analysis.select_control_rate(rows)

    def test_formal_matrix_is_exact_ab_ba_ab_and_one_worker(self):
        cells = self.runner.formal_cells(800)
        self.assertEqual(
            [(row["ordinal"], row["pair"], row["mode"]) for row in cells],
            [
                (1, 1, "face-serial"),
                (2, 1, "worker-serial"),
                (3, 2, "worker-serial"),
                (4, 2, "face-serial"),
                (5, 3, "face-serial"),
                (6, 3, "worker-serial"),
            ],
        )
        self.assertTrue(
            all(
                (row["warmup"], row["measure"], row["drain"]) == (10, 60, 10)
                for row in cells
            )
        )

    def test_receipt_ledger_is_once_only(self):
        with tempfile.TemporaryDirectory() as directory:
            ledger = self.runner.ReceiptLedger(Path(directory))
            receipt = {
                "schema": "spec138.receipt.v1",
                "ordinal": 1,
                "retryCount": 0,
            }
            ledger.append(receipt)
            with self.assertRaisesRegex(RuntimeError, "already exists"):
                ledger.append(receipt)
            stored = json.loads((Path(directory) / "01.json").read_text())
            self.assertEqual(stored["retryCount"], 0)

    def test_registered_necessity_decision(self):
        runs = [
            {"admissible": True, "fallbacks": 0, "shutdownDrained": True}
            for _ in range(6)
        ]
        pairs = [
            {
                "pairAdmissible": True,
                "faceCpuRelief": 0.55,
                "heartbeatP99Improvement": 0.25,
                "deliveryRatioChange": 0.0,
                "deliveryP99Improvement": 0.02,
            }
            for _ in range(3)
        ]
        result = self.analysis.classify(runs, pairs)
        self.assertEqual(result["verdict"], "NECESSARY_AT_TESTED_BOUNDARY")
        self.assertTrue(result["necessaryPredicate"])

    def test_face_relief_alone_is_not_called_necessary(self):
        runs = [
            {"admissible": True, "fallbacks": 0, "shutdownDrained": True}
            for _ in range(6)
        ]
        pairs = [
            {
                "pairAdmissible": True,
                "faceCpuRelief": 0.55,
                "heartbeatP99Improvement": 0.05,
                "deliveryRatioChange": 0.0,
                "deliveryP99Improvement": 0.0,
            }
            for _ in range(3)
        ]
        result = self.analysis.classify(runs, pairs)
        self.assertEqual(result["verdict"], "FACE_RELIEF_ONLY")
        self.assertFalse(result["necessaryPredicate"])

    def test_any_inadmissible_formal_cell_blocks_claim(self):
        runs = [
            {"admissible": True, "fallbacks": 0, "shutdownDrained": True}
            for _ in range(6)
        ]
        runs[0]["admissible"] = False
        pairs = [
            {
                "pairAdmissible": True,
                "faceCpuRelief": 0.60,
                "heartbeatP99Improvement": 0.30,
                "deliveryRatioChange": 0.0,
                "deliveryP99Improvement": 0.0,
            }
            for _ in range(3)
        ]
        self.assertEqual(
            self.analysis.classify(runs, pairs)["verdict"], "INADMISSIBLE"
        )


if __name__ == "__main__":
    unittest.main()
