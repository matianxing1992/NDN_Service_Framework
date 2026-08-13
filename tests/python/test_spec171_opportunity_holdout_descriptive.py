#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "spec171_holdout_descriptive",
    ROOT / "Experiments/summarize_spec171_opportunity_holdout_descriptive.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Spec171OpportunityHoldoutDescriptiveTest(unittest.TestCase):
    def make_primary(self):
        per_seed = []
        for seed in MODULE.EXPECTED_SEEDS:
            per_seed.append({
                "seed": seed,
                "ndnsf": {"requests": 10, "success": 10,
                           "attempts_or_executions": 10},
                "grpc": {"requests": 10, "success": 9,
                          "attempts_or_executions": 20},
                "nsc": {"requests": 10, "success": 8,
                         "attempts_or_executions": 30},
            })
        return {
            "schema": "spec171-opportunity-holdout-v1",
            "verdict": "HOLDOUT_CONFIRMS_CONDITIONAL_END_TO_END_ADVANTAGE",
            "per_seed": per_seed,
        }

    def test_summarizes_success_and_mechanism_counts(self):
        report = MODULE.summarize(self.make_primary())
        self.assertEqual(report["aggregate"]["ndnsf"]["successes"], 100)
        self.assertEqual(
            report["aggregate"]["grpc"]["attempts_or_executions_per_request"],
            2.0)
        difference = report[
            "paired_ndnsf_minus_baseline_success_rate_percentage_points"
        ]["grpc"]
        self.assertAlmostEqual(difference["mean"], 10.0)
        self.assertGreater(difference["ci95_low"], 0)

    def test_rejects_non_holdout_seed_set(self):
        primary = self.make_primary()
        primary["per_seed"].pop()
        with self.assertRaises(ValueError):
            MODULE.summarize(primary)


if __name__ == "__main__":
    unittest.main()
