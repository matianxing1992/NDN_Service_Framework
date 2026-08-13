#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "spec171_latency_mechanism",
    REPO_ROOT / "Experiments" / "analyze_spec171_latency_mechanism.py",
)
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


class Spec171LatencyMechanismTests(unittest.TestCase):
    def test_reconstructs_logical_latency_from_sequential_attempts(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "grpc-client.log"
            path.write_text(
                "\n".join([
                    "GRPC_FAILOVER_ATTEMPT request_id=0 attempt=1 provider=ucla "
                    "status=DEADLINE_EXCEEDED latency_ms=1000.0 handler_observed=0",
                    "GRPC_FAILOVER_ATTEMPT request_id=0 attempt=2 provider=wustl "
                    "status=UNAVAILABLE latency_ms=2.0 handler_observed=0",
                    "GRPC_FAILOVER_ATTEMPT request_id=0 attempt=3 provider=uiuc "
                    "status=OK latency_ms=18.0 server_provider=uiuc",
                    "GRPC_FAILOVER_ATTEMPT request_id=1 attempt=1 provider=wustl "
                    "status=OK latency_ms=20.0 server_provider=wustl",
                ]) + "\n",
                encoding="utf-8",
            )
            result = analysis.reconstruct_seed(path, 2, 1020.0)
        self.assertEqual(result["successful_after_deadline"], 1)
        self.assertEqual(result["successful_after_unavailable"], 1)
        self.assertEqual(result["successful_latency_bands"]["under_100_ms"], 1)
        self.assertEqual(result["successful_latency_bands"]["at_least_900_ms"], 1)
        self.assertEqual(result["attempt_status_counts"]["OK"], 2)
        self.assertEqual(result["maximum_attempts"], 3)

    def test_nearest_rank_matches_client_summary_rule(self):
        self.assertEqual(analysis.nearest_rank([30.0, 10.0, 20.0], 0.50), 20.0)
        self.assertEqual(analysis.nearest_rank([30.0, 10.0, 20.0], 0.95), 30.0)


if __name__ == "__main__":
    unittest.main()
