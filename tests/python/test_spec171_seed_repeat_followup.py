#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "seed_repeat_followup",
    REPO_ROOT / "Experiments" / "analyze_seed_repeat_followup.py",
)
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


class Spec171SeedRepeatFollowupTests(unittest.TestCase):
    PRIMARY = REPO_ROOT / "results" / "ndnsf-mobility-followup-20260807-primary-10seeds"
    REPEATS = REPO_ROOT / "results" / "ndnsf-mobility-followup-20260807-repeats"

    def test_primary_seed_gate_and_trace_matched_repeats(self):
        primary = analysis.records_by_seed(
            analysis.load(self.PRIMARY), analysis.PRIMARY_SEEDS)
        repeats = analysis.records_by_seed(
            analysis.load(self.REPEATS), analysis.REPEAT_SEEDS)
        result = analysis.summarize(primary, repeats)
        self.assertEqual(result["claim_verdict"], "NO_POSITIVE_MOBILITY_CONFIRMATION")
        self.assertAlmostEqual(
            result["systems"]["ndnsf"]["success_rate"], 0.745)
        self.assertAlmostEqual(
            result["paired_success_difference"]["grpc"]["mean"] * 100, 0.4)
        self.assertLess(result["paired_success_difference"]["grpc"]["lower"], 0.0)
        self.assertTrue(result["claim_gate"]["all_repeat_trace_hashes_match"])
        self.assertTrue(all(row["success_delta_pp"] == 0.0
                            for row in result["repeat_rows"]))


if __name__ == "__main__":
    unittest.main()
