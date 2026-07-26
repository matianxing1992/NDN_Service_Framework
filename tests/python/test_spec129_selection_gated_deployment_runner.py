#!/usr/bin/env python3

from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
EXPERIMENTS = REPO / "Experiments"
sys.path.insert(0, str(EXPERIMENTS))

from run_spec129_selection_gated_deployment_matrix import (
    METRIC_SCHEMA, analyze_cell, frozen_cells, owner_processes,
    validate_campaign_not_frozen, validate_live_invoker, validate_manifest,
)


class Spec129RunnerTest(unittest.TestCase):
    def test_manifest_is_exactly_twelve_unique_nonrerunnable_cells(self):
        cells = frozen_cells()
        validate_manifest(cells)
        self.assertEqual([item["cell"] for item in cells], list(range(1, 13)))
        self.assertEqual(len({item["id"] for item in cells}), 12)
        self.assertTrue(all(not item["automaticRetry"] and not item["rerunAllowed"]
                            for item in cells))

    def test_manifest_rejects_missing_duplicate_and_rerunnable_cells(self):
        with self.assertRaises(RuntimeError):
            validate_manifest(frozen_cells()[:-1])
        duplicate = frozen_cells()
        duplicate[-1] = dict(duplicate[0], cell=12)
        with self.assertRaises(RuntimeError):
            validate_manifest(duplicate)
        rerunnable = frozen_cells()
        rerunnable[0] = dict(rerunnable[0], rerunAllowed=True)
        with self.assertRaises(RuntimeError):
            validate_manifest(rerunnable)

    def test_analyzer_requires_complete_attribution_and_plaintext_scan(self):
        cell = frozen_cells()[0]
        metrics = {name: 0 for name in METRIC_SCHEMA}
        metrics.update({"requestCount": 1, "ackCount": 3,
                        "reservationCreatedCount": 3,
                        "decisionSelectedCount": 1,
                        "decisionNotSelectedCount": 2,
                        "cleanupCount": 3,
                        "selectedPreparationCount": 1, "executionCount": 1})
        raw = {"scenario": cell["id"], "passed": True, "invocationCount": 1,
               "metrics": metrics, "checks": {"exactMembership": True}}
        self.assertTrue(analyze_cell(cell, raw, 0)["accepted"])
        del raw["metrics"]["decisionReceiptCount"]
        self.assertFalse(analyze_cell(cell, raw, 0)["accepted"])
        raw["metrics"]["decisionReceiptCount"] = 3
        raw["metrics"]["packetPlaintextMatches"] = 1
        self.assertFalse(analyze_cell(cell, raw, 0)["accepted"])

    def test_root_and_single_owner_guards(self):
        with self.assertRaises(PermissionError):
            validate_live_invoker(1000)
        validate_live_invoker(0)
        sample = "200 1 S 3 root python3 NDNSF_DI_SelectionGatedDeployment_Minindn.py\n"
        self.assertEqual(owner_processes(sample), [sample.strip()])

    def test_closed_spec_rejects_live_campaign_but_allows_dry_validation(self):
        with self.assertRaisesRegex(RuntimeError, "formal matrix is frozen"):
            validate_campaign_not_frozen(dry_run=False)
        validate_campaign_not_frozen(dry_run=True)

    def test_dry_run_freezes_paths_hashes_and_zero_invocations(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "spec129-dry-run"
            completed = subprocess.run([
                sys.executable,
                str(EXPERIMENTS / "run_spec129_selection_gated_deployment_matrix.py"),
                "--output-root", str(output), "--dry-run",
            ], cwd=REPO, text=True, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            summary = json.loads((output / "campaign-summary.json").read_text())
            manifest = json.loads((output / "campaign-manifest.json").read_text())
            self.assertEqual(summary["status"], "DRY_RUN")
            self.assertEqual(len(manifest), 12)
            self.assertTrue(summary["spec128HashesBefore"])
            self.assertTrue(all(item["invocationCount"] == 0 for item in manifest))

    def test_output_reuse_and_spec128_destination_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "spec129-existing"
            output.mkdir()
            completed = subprocess.run([
                sys.executable,
                str(EXPERIMENTS / "run_spec129_selection_gated_deployment_matrix.py"),
                "--output-root", str(output), "--dry-run",
            ], cwd=REPO, text=True, capture_output=True)
            self.assertNotEqual(completed.returncode, 0)
        completed = subprocess.run([
            sys.executable,
            str(EXPERIMENTS / "run_spec129_selection_gated_deployment_matrix.py"),
            "--output-root", "/tmp/spec128-forbidden", "--dry-run",
        ], cwd=REPO, text=True, capture_output=True)
        self.assertNotEqual(completed.returncode, 0)


if __name__ == "__main__":
    unittest.main()
