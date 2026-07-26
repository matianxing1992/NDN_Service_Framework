#!/usr/bin/env python3

from pathlib import Path
import json
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "specs/130-concurrent-fault-boundaries/experiment-manifest.json"
sys.path.insert(0, str(ROOT / "Experiments"))

from run_spec130_concurrent_fault_matrix import (
    METRIC_SCHEMA, analyze_cell, frozen_cells, load_manifest, owner_processes,
    validate_live_invoker, validate_manifest,
)


class Spec130ManifestTest(unittest.TestCase):
    def test_manifest_expands_to_fresh_exact_once_paired_cells(self):
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        cells = frozen_cells(manifest)
        self.assertEqual([cell["cell"] for cell in cells], list(range(1, 69)))
        self.assertEqual(len({cell["id"] for cell in cells}), 68)
        self.assertFalse(manifest["automaticRetry"])
        self.assertFalse(manifest["rerunAllowed"])
        self.assertEqual(manifest["spec129Policy"], "hash-only-never-invoke")
        self.assertEqual(len(manifest["statistics"]["seeds"]), 22)
        self.assertIn("failed", manifest["statistics"]["failedTrialPolicy"])
        self.assertEqual(sum("seed" in cell for cell in cells), 44)

    def test_manifest_covers_graph_modes_and_failure_boundaries(self):
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        cells = frozen_cells(manifest)
        self.assertTrue({"disjoint", "identical", "nested", "partial", "cycle"}
                        <= {cell["graph"] for cell in cells})
        self.assertEqual({"centralized", "lease-only"},
                         {cell["mode"] for cell in cells})
        faults = {cell["fault"] for cell in cells}
        self.assertTrue({"requester-after-reserve", "provider-after-decision",
                         "authority-before-grant", "asymmetric-partition",
                         "equal-expiry", "provider-restart", "split-brain",
                         "capability-mismatch"} <= faults)
        serialized = MANIFEST.read_text(encoding="utf-8").lower()
        self.assertNotIn("run_spec129", serialized)
        self.assertNotIn("uav", serialized)
        self.assertNotIn("codec", serialized)

    def test_runner_manifest_validation_rejects_reuse_and_missing_coverage(self):
        manifest = load_manifest(); validate_manifest(manifest)
        duplicate = json.loads(json.dumps(manifest))
        duplicate["cells"][-1]["id"] = duplicate["cells"][-2]["id"]
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            validate_manifest(duplicate)
        missing = json.loads(json.dumps(manifest))
        for value in missing["cells"]:
            if value["graph"] == "disjoint":
                value["graph"] = "identical"
        with self.assertRaisesRegex(RuntimeError, "coverage"):
            validate_manifest(missing)
        retry = json.loads(json.dumps(manifest)); retry["automaticRetry"] = True
        with self.assertRaisesRegex(RuntimeError, "retry"):
            validate_manifest(retry)

    def test_analyzer_keeps_expected_unavailability_and_reports_mapping_ratio(self):
        manifest = load_manifest()
        cell = next(value for value in frozen_cells(manifest)
                    if value["fault"] == "asymmetric-partition")
        metrics = {name: 0 for name in METRIC_SCHEMA}
        metrics.update({"mappingDataCount": 4, "newMappingDataCount": 1,
                        "returnedNewMappingRatio": .25,
                        "unavailableCount": 1})
        raw = {"scenario": cell["id"], "passed": True, "invocationCount": 1,
               "manifestDigest": __import__("hashlib").sha256(
                   MANIFEST.read_bytes()).hexdigest(),
               "metrics": metrics, "checks": {"realMiniNDN": True}}
        row = analyze_cell(cell, raw, 0, manifest)
        self.assertTrue(row["accepted"])
        self.assertEqual(row["availabilityOutcome"], "unavailable")
        raw["metrics"]["returnedNewMappingRatio"] = .5
        self.assertFalse(analyze_cell(cell, raw, 0, manifest)["accepted"])
        del raw["metrics"]["payloadDataCount"]
        self.assertFalse(analyze_cell(cell, raw, 0, manifest)["accepted"])

    def test_root_and_single_writer_process_guards(self):
        with self.assertRaises(PermissionError):
            validate_live_invoker(1000)
        validate_live_invoker(0)
        process = "200 1 S 3 root python3 NDNSF_DI_ConcurrentFaultBoundaries_Minindn.py"
        self.assertEqual(owner_processes(process), [process])
        old = "201 1 S 3 root python3 run_spec129_selection_gated_deployment_matrix.py"
        self.assertEqual(owner_processes(old), [old])


if __name__ == "__main__":
    unittest.main()
