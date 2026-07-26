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

from run_spec127_cross_application_matrix import (
    ROOT,
    analyze_cell_summary,
    digest_path,
    frozen_cells,
    owner_processes,
    validate_live_invoker,
)


class Spec127RunnerTest(unittest.TestCase):
    def test_live_campaign_requires_one_root_owner(self):
        with self.assertRaisesRegex(PermissionError, "run the campaign as root"):
            validate_live_invoker(1000)
        validate_live_invoker(0)

    def test_accepted_core_and_binding_sources_match_spec126_confirmation(self):
        expected = {
            "ndn-service-framework/Stream.hpp":
                "b55d32e8f8d63612a3e728e3bc37b176b59ddf2ee8e8078218ca069ec78ca01c",
            "ndn-service-framework/Stream.cpp":
                "4ac97b25d6d69c6723c890954be300ad3cc4c819072d40f197810f0da48333f2",
            "pythonWrapper/src/ndnsf/_ndnsf.cpp":
                "30ea806747caf429127443b7938aebfec79e849f7503052e2b4de23c212b0d2f",
            "pythonWrapper/ndnsf/streaming.py":
                "9ad7a91aa86cd8b998d0a66d842a6d801ec305722f2888334c7594d6e04574a2",
        }
        actual = {name: digest_path(ROOT / name) for name in expected}
        self.assertEqual(actual, expected)

    def test_manifest_has_exactly_twelve_unique_one_shot_cells(self):
        cells = frozen_cells()
        self.assertEqual(len(cells), 12)
        self.assertEqual(len({cell["id"] for cell in cells}), 12)
        self.assertEqual(
            {(cell["workloadId"], cell["networkProfile"]):
             sum(value["workloadId"] == cell["workloadId"] and
                 value["networkProfile"] == cell["networkProfile"]
                 for value in cells)
             for cell in cells},
            {("periodic-sensor", "zero-loss"): 1,
             ("periodic-sensor", "combined"): 5,
             ("variable-multisegment", "zero-loss"): 1,
             ("variable-multisegment", "combined"): 5})
        self.assertTrue(all(not cell["automaticRetry"] for cell in cells))
        self.assertTrue(all(not cell["rerunAllowed"] for cell in cells))

    def test_analyzer_reports_all_traffic_classes_and_unavailable_ratios(self):
        raw = {
            "passed": False,
            "providerStatus": {"necessarySourceRepairItems": 0,
                               "nativeStatus": {"provider_future_interests": 0,
                                                "provider_future_hits": 0}},
            "consumerStatus": {
                "expectedMeasuredSamples": 600, "completeMeasuredSamples": 0,
                "duplicates": 0, "partialSamples": 0,
                "outOfOrderSamples": 0, "publicationToDeliveryMs": [],
                "receipts": [],
                "nativeStatus": {
                    "payload_interests": 0, "mapping_interests": 3,
                    "mapping_data_responses": 0,
                    "mapping_new_data_responses": 0, "mapping_bytes": 0,
                    "retry_attempts": 2, "timeouts": 1, "nacks": 4}},
        }
        run = analyze_cell_summary(frozen_cells()[0], raw, 2)
        self.assertIsNone(run["payloadInterestOverheadRatio"])
        self.assertEqual(run["payloadInterestOverheadRatioUnavailableReason"],
                         "no-necessary-source-repair-items")
        self.assertIsNone(run["mappingNewDataRatio"])
        self.assertIsNone(run["providerFutureHitRatio"])
        self.assertEqual((run["retryAttempts"], run["timeouts"], run["nacks"]),
                         (2, 1, 4))
        self.assertEqual(run["trafficCounterScope"],
                         "full-run-including-warmup")
        self.assertFalse(run["accepted"])

    def test_dry_run_freezes_commands_hashes_and_unique_outputs_without_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "campaign"
            completed = subprocess.run(
                [sys.executable,
                 str(EXPERIMENTS / "run_spec127_cross_application_matrix.py"),
                 "--output-root", str(output), "--duration-seconds", "60",
                 "--dry-run"], cwd=REPO, text=True, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            summary = json.loads(
                (output / "campaign-summary.json").read_text(encoding="utf-8"))
            manifest = json.loads(
                (output / "campaign-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["status"], "DRY_RUN")
        self.assertEqual(len(manifest), 12)
        self.assertEqual(len({entry["cell"]["outputPath"] for entry in manifest}), 12)
        self.assertTrue(summary["sourceHashes"])
        self.assertEqual(set(summary["workloadHashes"]),
                         {"periodic-sensor", "variable-multisegment"})

    def test_owner_scan_filters_launcher_ancestry_and_detects_live_owners(self):
        sample = (
            "101 1 S 10 user python3 unrelated.py\n"
            "150 1 S 10 user bash -lc python3 run_spec127_cross_application_matrix.py\n"
            "202 1 S 20 user python3 Experiments/NDNSF_LiveStream_Generality_"
            "Minindn.py --output /tmp/live\n")
        self.assertEqual(owner_processes(
            sample, current_pid=999, ignored_pids={150}),
            [sample.splitlines()[2]])


if __name__ == "__main__":
    unittest.main()
