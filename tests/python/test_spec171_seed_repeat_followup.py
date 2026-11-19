#!/usr/bin/env python3
"""Synthetic analysis regressions, never historical mobility evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "seed_repeat_followup", ROOT / "Experiments/analyze_seed_repeat_followup.py")
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


class SeedRepeatAnalysisTests(unittest.TestCase):
    def reports(self, root):
        records = []
        for seed in analysis.PRIMARY_SEEDS:
            for system, success in (("ndnsf", 75), ("grpc", 80), ("nsc", 50)):
                manifest = root / (str(seed) + "-" + system + ".json")
                manifest.write_text(json.dumps({"trace_sha256": "synthetic-trace-" + str(seed)}))
                records.append(dict(condition=analysis.CONDITION, status="complete", seed=seed,
                    system=system, cell_manifest=str(manifest),
                    summary=dict(sent=100, success=success, mean_ms=10.0)))
        return (dict(complete_cells=30, total_cells=30, records=records),
                dict(complete_cells=9, total_cells=9,
                     records=[dict(row) for row in records if row["seed"] in analysis.REPEAT_SEEDS]))

    def summarize(self, primary, repeats):
        return analysis.summarize(
            analysis.records_by_seed(primary, analysis.PRIMARY_SEEDS),
            analysis.records_by_seed(repeats, analysis.REPEAT_SEEDS))

    def test_synthetic_pairing_and_matching_repeats(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            primary, repeats = self.reports(root)
            path = root / "aggregate.json"
            path.write_text(json.dumps(primary))
            result = self.summarize(analysis.load(path), repeats)
            self.assertAlmostEqual(result["systems"]["ndnsf"]["success_rate"], 0.75)
            self.assertAlmostEqual(result["paired_success_difference"]["grpc"]["mean"], -0.05)
            self.assertLess(result["paired_success_difference"]["grpc"]["lower"], 0)
            self.assertTrue(result["claim_gate"]["all_repeat_trace_hashes_match"])
            self.assertTrue(all(row["success_delta_pp"] == 0 for row in result["repeat_rows"]))

    def test_synthetic_trace_mismatch_is_not_an_independent_seed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            primary, repeats = self.reports(root)
            changed = root / "changed.json"
            changed.write_text(json.dumps({"trace_sha256": "synthetic-different-trace"}))
            repeats["records"][0]["cell_manifest"] = str(changed)
            result = self.summarize(primary, repeats)
            self.assertFalse(result["claim_gate"]["all_repeat_trace_hashes_match"])
            self.assertEqual(len(result["paired_success_difference"]["grpc"]["values"]), 10)

    def test_incomplete_aggregate_and_missing_system_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            primary, _ = self.reports(root)
            path = root / "aggregate.json"
            path.write_text(json.dumps(dict(primary, complete_cells=29)))
            with self.assertRaisesRegex(ValueError, "incomplete aggregate"):
                analysis.load(path)
            primary["records"].pop()
            with self.assertRaisesRegex(ValueError, "is incomplete"):
                analysis.records_by_seed(primary, analysis.PRIMARY_SEEDS)


if __name__ == "__main__":
    unittest.main()
