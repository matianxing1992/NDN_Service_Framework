import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "Experiments/analyze_spec171_opportunity_windows.py"
SPEC = importlib.util.spec_from_file_location("spec171_opportunity", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Spec171OpportunityWindowsTest(unittest.TestCase):
    def test_classification_is_mutually_exclusive(self):
        self.assertEqual(
            MODULE.classify_opportunity({name: False for name in MODULE.PROVIDERS}, "ucla"),
            "NONE_REACHABLE")
        self.assertEqual(MODULE.classify_opportunity(
            {"ucla": True, "wustl": True, "uiuc": False, "arizona": False},
            "ucla"), "INITIAL_REACHABLE")
        self.assertEqual(MODULE.classify_opportunity(
            {"ucla": False, "wustl": True, "uiuc": False, "arizona": False},
            "ucla"), "SWITCH_REQUIRED")

    def test_target_order_comes_from_retained_runtime_command(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime-commands.json"
            path.write_text(
                '{"client":"client --target ucla=1 --target wustl=2 '
                '--target uiuc=3 --target arizona=4"}')
            self.assertEqual(MODULE.parse_target_order(path), list(MODULE.PROVIDERS))

    def test_grpc_parser_separates_failed_attempt_cost(self):
        with tempfile.TemporaryDirectory() as directory:
            cell = Path(directory)
            (cell / "grpc-client.log").write_text(
                "GRPC_FAILOVER_ATTEMPT request_id=0 attempt=1 provider=ucla "
                "status=DEADLINE_EXCEEDED latency_ms=1000.0 handler_observed=0\n"
                "GRPC_FAILOVER_ATTEMPT request_id=0 attempt=2 provider=wustl "
                "status=OK latency_ms=10.0 server_provider=wustl\n")
            result = MODULE.parse_grpc_requests(cell)[0]
            self.assertEqual(result["terminal_status"], "SUCCESS")
            self.assertEqual(result["failed_attempt_time_ms"], 1000.0)
            self.assertEqual(result["user_latency_ms"], 1010.0)
            self.assertEqual(result["attempts_or_provider_executions"], 2)

    def test_paired_bootstrap_is_fixed_seed_and_directional(self):
        first = MODULE.paired_bootstrap_mean(
            [900.0, 800.0, 700.0, 600.0, 500.0], seed=171, repetitions=2000)
        second = MODULE.paired_bootstrap_mean(
            [900.0, 800.0, 700.0, 600.0, 500.0], seed=171, repetitions=2000)
        self.assertEqual(first, second)
        self.assertGreater(first["ci95_low"], 0)


if __name__ == "__main__":
    unittest.main()
