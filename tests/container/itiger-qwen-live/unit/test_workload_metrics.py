from __future__ import annotations

import json
import unittest

from _support import FIXTURES, load_tool


workload = load_tool("spec110_workload")


class WorkloadMetricTests(unittest.TestCase):
    def test_frozen_fixture_and_exact_tokens(self):
        fixture = json.loads((FIXTURES / "workload.json").read_text())
        workload.validate_workload(fixture)
        passed = workload.exact_token_record([1, 2], [3, 4], [3, 4])
        failed = workload.exact_token_record([1, 2], [3, 4], [3, 5])
        self.assertTrue(passed["exactMatch"])
        self.assertFalse(failed["exactMatch"])
        self.assertNotEqual(failed["oracleDigest"], failed["outputDigest"])

    def test_warmup_is_outside_half_open_sixty_second_window(self):
        window = workload.MeasurementWindow(warmup_requests=3, duration_seconds=60)
        self.assertEqual([window.record(t, t) for t in (0, 1, 2)], ["WARMUP_EXCLUDED"] * 3)
        self.assertEqual(window.record(10, 1), "MEASURED")
        self.assertEqual(window.record(69.999, 2), "MEASURED")
        self.assertEqual(window.record(70.0, 3), "OUTSIDE_MEASURED_WINDOW")
        report = window.report()
        self.assertEqual(report["startTimeSeconds"], 10)
        self.assertEqual(report["endTimeSeconds"], 70)
        self.assertEqual(report["sampleCount"], 2)

    def test_percentiles_require_exact_minimum_sample_counts(self):
        samples = list(range(1000))
        for name, threshold in workload.PERCENTILE_REQUIREMENTS.items():
            self.assertEqual(workload.percentile(samples[:threshold - 1], name)["status"], "UNAVAILABLE_INSUFFICIENT_SAMPLES")
            self.assertEqual(workload.percentile(samples[:threshold], name)["status"], "AVAILABLE")

    def test_sampling_is_stable_and_seed_bound(self):
        first = [workload.deterministic_sample(f"request-{i}", 0.25, seed="a") for i in range(100)]
        second = [workload.deterministic_sample(f"request-{i}", 0.25, seed="a") for i in range(100)]
        changed = [workload.deterministic_sample(f"request-{i}", 0.25, seed="b") for i in range(100)]
        self.assertEqual(first, second)
        self.assertNotEqual(first, changed)


if __name__ == "__main__":
    unittest.main()
