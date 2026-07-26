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

from run_spec128_generic_recovery_matrix import (
    METRIC_SCHEMA,
    analyze_cell_summary,
    frozen_cells,
    owner_processes,
    validate_live_invoker,
)


class Spec128RunnerTest(unittest.TestCase):
    def test_manifest_is_exactly_frozen_two_plus_ten_plus_four(self):
        cells = frozen_cells()
        self.assertEqual(len(cells), 16)
        self.assertEqual(len({value["id"] for value in cells}), 16)
        counts = {}
        for value in cells:
            key = value["networkProfile"]
            counts[key] = counts.get(key, 0) + 1
            self.assertFalse(value["automaticRetry"])
            self.assertFalse(value["rerunAllowed"])
            self.assertEqual(value["maximumAttemptsPerCursor"], 3)
        self.assertEqual(counts, {"zero-loss": 2, "multi-loss-retry": 10,
                                  "capacity-plus-one": 4})
        variable = [value for value in cells
                    if value["workloadId"] == "variable-multisegment"]
        self.assertTrue(all(value["declaredRecoveryCapacity"] == 2
                            for value in variable))

    def test_live_campaign_requires_one_root_owner(self):
        with self.assertRaisesRegex(PermissionError, "root"):
            validate_live_invoker(1000)
        validate_live_invoker(0)

    def test_owner_scan_detects_other_launcher_but_ignores_ancestry(self):
        sample = (
            "150 1 S 10 root python3 run_spec128_generic_recovery_matrix.py\n"
            "202 1 S 20 root python3 NDNSF_LiveStream_Generality_Minindn.py\n")
        self.assertEqual(owner_processes(sample, current_pid=999,
                                         ignored_pids={150}),
                         [sample.splitlines()[1]])

    def test_analyzer_reports_every_required_traffic_class(self):
        cell = frozen_cells()[0]
        raw = {
            "passed": True, "providerReturnCode": 0,
            "effectiveQdiscBeforeApps": {
                "provider": {"show": "netem delay 1ms"},
                "consumer": {"show": "netem delay 1ms"}},
            "providerStatus": {"necessarySourceRepairItems": 10,
                "nativeStatus": {"provider_future_interests": 10,
                    "provider_future_hits": 10,
                    "provider_initial_future_interests": 10,
                    "provider_initial_future_hits": 10,
                    "provider_retry_future_interests": 0,
                    "provider_retry_future_hits": 0}},
            "consumerStatus": {"expectedMeasuredSamples": 10,
                "completeMeasuredSamples": 10, "duplicates": 0,
                "partialSamples": 0, "outOfOrderSamples": 0,
                "invalidItems": 0, "skipsByReason": {},
                "measuredPublicationToDeliveryMs": [10.0] * 10,
                "receipts": [{"phase": "measured",
                    "completed_timestamp_us": index * 100_000}
                    for index in range(10)],
                "nativeStatus": {"payload_interests": 10,
                    "initial_payload_interests": 10,
                    "retry_payload_interests": 0,
                    "initial_future_payload_interests": 10,
                    "retry_future_payload_interests": 0,
                    "retry_successes": 0, "retry_exhaustions": 0,
                    "retry_suppressions": 0, "mapping_interests": 2,
                    "mapping_data_responses": 2,
                    "mapping_new_data_responses": 2, "mapping_bytes": 200,
                    "timeouts": 0, "nacks": 0,
                    "declared_recovery_capacity": 0,
                    "recovery_attempts": 0, "recovered": 0,
                    "recovery_exhaustions": 0}},
        }
        run = analyze_cell_summary(cell, raw, 0)
        self.assertTrue(run["accepted"], run["checks"])
        self.assertEqual(run["initialPayloadInterests"], 10)
        self.assertEqual(run["retryPayloadInterests"], 0)
        self.assertEqual(run["providerInitialFutureHits"], 10)
        self.assertTrue(set(METRIC_SCHEMA) - set(run) <=
                        set(run["unavailableMetrics"]))

    def test_dry_run_freezes_unique_paths_and_spec127_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "spec128-dry-run"
            completed = subprocess.run([
                sys.executable,
                str(EXPERIMENTS / "run_spec128_generic_recovery_matrix.py"),
                "--output-root", str(output), "--duration-seconds", "60",
                "--dry-run"], cwd=REPO, text=True, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            summary = json.loads((output / "campaign-summary.json").read_text())
            manifest = json.loads((output / "campaign-manifest.json").read_text())
        self.assertEqual(summary["status"], "DRY_RUN")
        self.assertEqual(len(manifest), 16)
        self.assertEqual(len({entry["cell"]["outputPath"] for entry in manifest}), 16)
        self.assertTrue(summary["spec127Hashes"])
        self.assertTrue(all(entry["invocationCount"] == 0 for entry in manifest))

    def test_spec127_destination_is_rejected(self):
        completed = subprocess.run([
            sys.executable,
            str(EXPERIMENTS / "run_spec128_generic_recovery_matrix.py"),
            "--output-root", "/tmp/spec127-forbidden", "--dry-run"],
            cwd=REPO, text=True, capture_output=True)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("immutable", completed.stderr)


if __name__ == "__main__":
    unittest.main()
