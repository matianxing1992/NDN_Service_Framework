#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "spec171_holdout",
    ROOT / "Experiments/analyze_spec171_opportunity_holdout.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Spec171OpportunityHoldoutTest(unittest.TestCase):
    def test_gate_state_rejects_non_atomic_epoch(self):
        trace = {
            "events": [
                (1.000, "ucla", True), (1.001, "wustl", False),
                (1.002, "uiuc", True), (1.003, "arizona", True)],
            "transition_windows": [(1.000, 1.003)],
        }
        self.assertIsNone(MODULE.gate_state_at(trace, 1.0015))
        state = MODULE.gate_state_at(trace, 1.050)
        self.assertEqual(
            MODULE.classify(state, "wustl"), "SWITCH_REQUIRED")

    def test_all_three_parsers_retain_publication_and_latency(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "ndnsf-user.log").write_text(
                "INTERMITTENT_USER_REQUEST_RESULT request_index=0 requestId=/id "
                "status=SUCCESS latency_ms=12.500 published_monotonic_ms=1000.250\n")
            (root / "grpc-client.log").write_text(
                "GRPC_REQUEST_PUBLISHED request_id=0 monotonic_s=1.000250000\n"
                "GRPC_FAILOVER_ATTEMPT request_id=0 attempt=1 provider=ucla "
                "status=OK latency_ms=8.500\n")
            (root / "nsc-consumer.log").write_text(
                "NSC_REQUEST_RESULT request_id=1 status=SUCCESS latency_ms=20.000 "
                "attempts=1 provider=/muas/ucla published_monotonic_ms=1000.250\n")
            nd = MODULE.parse_ndnsf(root)[0]
            grpc = MODULE.parse_grpc(root)[0]
            nsc = MODULE.parse_nsc(root)[0]
        self.assertEqual((nd["latency_ms"], grpc["latency_ms"], nsc["latency_ms"]),
                         (12.5, 8.5, 20.0))
        self.assertEqual((nd["published_monotonic_s"],
                          grpc["published_monotonic_s"],
                          nsc["published_monotonic_s"]),
                         (1.00025, 1.00025, 1.00025))

    def test_paired_bootstrap_is_fixed_and_directional(self):
        result = MODULE.paired_bootstrap_mean([-10.0] * 10)
        self.assertEqual(result["mean"], -10.0)
        self.assertLess(result["ci95_high"], 0)


if __name__ == "__main__":
    unittest.main()
