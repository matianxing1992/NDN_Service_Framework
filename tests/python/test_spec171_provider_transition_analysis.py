import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "Experiments/analyze_spec171_provider_transition.py"
SPEC = importlib.util.spec_from_file_location("spec171_transition_analysis", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Spec171ProviderTransitionAnalysisTest(unittest.TestCase):
    def test_registered_phase_boundaries(self):
        self.assertEqual(MODULE.phase_for(0), "initial")
        self.assertEqual(MODULE.phase_for(79), "initial")
        self.assertEqual(MODULE.phase_for(80), "transition_boundary")
        self.assertEqual(MODULE.phase_for(81), "overlap")
        self.assertEqual(MODULE.phase_for(179), "overlap")
        self.assertEqual(MODULE.phase_for(180), "transition_boundary")
        self.assertEqual(MODULE.phase_for(181), "post_retirement")
        self.assertEqual(MODULE.phase_for(299), "post_retirement")

    def test_window_summary_retains_attempts_and_provider_d(self):
        requests = {
            index: {
                "success": True,
                "latency_ms": 10.0,
                "attempts": 2,
                "provider_d": index >= 180,
            }
            for index in range(300)
        }
        summary = MODULE.summarize_requests(requests)
        self.assertEqual(summary["initial"]["requests"], 80)
        self.assertEqual(summary["transition_boundary"]["requests"], 2)
        self.assertEqual(summary["overlap"]["requests"], 99)
        self.assertEqual(summary["post_retirement"]["requests"], 119)
        self.assertEqual(summary["post_retirement"]["provider_d_successes"], 119)
        self.assertEqual(summary["post_retirement"]["attempts_or_executions"], 238)

    def test_grpc_parser_retains_actual_publication_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            cell = Path(temporary)
            (cell / "grpc-client.log").write_text(
                "GRPC_REQUEST_PUBLISHED request_id=180 monotonic_s=123.456789000\n"
                "GRPC_FAILOVER_ATTEMPT request_id=180 attempt=1 provider=ucla "
                "status=OK latency_ms=9.091 server_provider=ucla\n")
            requests = MODULE.parse_grpc(cell)
        self.assertEqual(requests[180]["published_monotonic_s"], 123.456789)

    def test_actual_gate_state_excludes_non_atomic_transition(self):
        transitions = [
            (10.000, "ucla", True),
            (10.001, "wustl", True),
            (10.002, "uiuc", True),
            (10.003, "arizona", True),
            (20.000, "ucla", False),
            (20.010, "wustl", False),
            (20.020, "uiuc", False),
            (20.021, "arizona", True),
        ]
        self.assertEqual(MODULE.phase_for_actual(19.9, transitions), "overlap")
        self.assertEqual(
            MODULE.phase_for_actual(20.015, transitions), "transition_boundary")
        self.assertEqual(
            MODULE.phase_for_actual(20.1, transitions), "post_retirement")


if __name__ == "__main__":
    unittest.main()
