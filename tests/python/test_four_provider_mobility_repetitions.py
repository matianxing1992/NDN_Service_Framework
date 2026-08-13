import importlib.util
from pathlib import Path
import types
import unittest
from unittest.mock import patch


MODULE_PATH = Path(__file__).resolve().parents[2] / "Experiments" / "four_provider_mobility_repetitions.py"
SPEC = importlib.util.spec_from_file_location("four_provider_mobility_repetitions", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def metric(success, attempts):
    return {
        "sent": 10,
        "success": success,
        "success_rate": success / 10.0,
        "deadline_failures": 10 - success,
        "attempts": attempts,
        "attempts_per_request": attempts / 10.0,
        "failovers": max(0, attempts - 10),
        "failovers_per_request": max(0, attempts - 10) / 10.0,
        "p50_ms": None,
        "p95_ms": None,
        "p99_ms": None,
        "provider_executions": None,
        "ndnsf_strategy": "first-responding",
    }


class FourProviderRepetitionTests(unittest.TestCase):
    def args(self):
        return types.SimpleNamespace(
            bootstrap_iterations=1000,
            bootstrap_seed=171,
            min_repetitions=2,
            seeds=[20, 21],
            duration_s=60,
            rate_rps=5.0,
            processing_delay_ms=5,
            ack_timeout_ms=200,
            ndnsf_strategy="first-responding",
            trace_profile="single-active-handoff",
        )

    def test_command_contains_shared_three_system_contract(self):
        args = types.SimpleNamespace(
            harness=str(MODULE_PATH), lock_file="/tmp/test-mobility.lock",
            duration_s=60, rate_rps=5.0, processing_delay_ms=5,
            ack_timeout_ms=200, traffic_start_delay_s=2.0, settle_seconds=5,
            ndnsf_strategy="first-responding", service_workers=4,
        )
        command = MODULE.command_for(args, "stale-health", 20, Path("/tmp/cell"))
        self.assertIn("--systems", command)
        self.assertIn("ndnsf,grpc,nsc", command)
        self.assertIn("--include-ndnsf", command)
        self.assertIn("--timeout-ms", command)
        self.assertEqual(command[command.index("--timeout-ms") + 1], "300")
        self.assertEqual(command[command.index("--service-workers") + 1], "4")

    def test_strict_grpc_flag_reaches_child_harness_command(self):
        args = types.SimpleNamespace(
            harness=str(MODULE_PATH), lock_file="/tmp/test-mobility.lock",
            duration_s=60, rate_rps=5.0, processing_delay_ms=5,
            ack_timeout_ms=200, traffic_start_delay_s=4.0, settle_seconds=5,
            ndnsf_strategy="first-responding", trace_profile="single-active-handoff",
            handoff_period_s=1.0, grpc_no_health_routing=True,
        )
        command = MODULE.command_for(args, "stale-health", 20, Path("/tmp/cell"))
        self.assertIn("--grpc-no-health-routing", command)

    def test_claim_requires_lower_confidence_bound_and_attempt_gate(self):
        records = []
        for condition in MODULE.CONDITIONS:
            for seed in (20, 21):
                records.append({
                    "condition": condition,
                    "seed": seed,
                    "status": "complete",
                    "systems": {
                        "ndnsf": metric(10, 10),
                        "grpc": metric(8, 10),
                        "nsc": metric(7, 12),
                    },
                })
        report = MODULE.aggregate(records, self.args())
        self.assertEqual(report["claim_verdict"], "NDNSF_MOBILITY_ADVANTAGE")
        self.assertEqual(
            report["supplementary_verdict"], "NDNSF_REDUNDANT_COVERAGE_ADVANTAGE")

        records[2]["systems"]["ndnsf"] = metric(9, 100)
        report = MODULE.aggregate(records, self.args())
        self.assertEqual(report["claim_verdict"], "NO_DEMONSTRATED_ADVANTAGE")
        self.assertEqual(
            report["supplementary_verdict"],
            "NO_DEMONSTRATED_REDUNDANT_COVERAGE_ADVANTAGE")

    def test_missing_triplet_is_inconclusive(self):
        records = [{
            "condition": "moderate", "seed": 20, "status": "complete",
            "systems": {system: metric(10, 10) for system in MODULE.SYSTEMS},
        }]
        report = MODULE.aggregate(records, self.args())
        self.assertEqual(report["claim_verdict"], "INCONCLUSIVE_MISSING_CELL")

    def test_random_waypoint_advantage_is_supplementary_only(self):
        records = []
        args = self.args()
        args.trace_profile = "random-waypoint"
        for condition in MODULE.CONDITIONS:
            for seed in (20, 21):
                records.append({
                    "condition": condition,
                    "seed": seed,
                    "status": "complete",
                    "systems": {
                        "ndnsf": metric(10, 10),
                        "grpc": metric(8, 10),
                        "nsc": metric(7, 12),
                    },
                })
        report = MODULE.aggregate(records, args)
        self.assertEqual(report["claim_verdict"], "NO_DEMONSTRATED_ADVANTAGE")
        self.assertEqual(
            report["supplementary_verdict"], "NDNSF_REDUNDANT_COVERAGE_ADVANTAGE")

    def test_disk_budget_rejects_insufficient_free_space(self):
        with patch.object(MODULE.shutil, "disk_usage",
                          return_value=types.SimpleNamespace(free=2 * 1024 ** 3)):
            with self.assertRaisesRegex(RuntimeError, "requires at least 5.00 GiB"):
                MODULE.enforce_disk_budget(Path("/tmp"), 5.0, "before campaign")

    def test_disk_budget_accepts_margin_and_zero_disables_threshold(self):
        with patch.object(MODULE.shutil, "disk_usage",
                          return_value=types.SimpleNamespace(free=6 * 1024 ** 3)):
            self.assertEqual(
                MODULE.enforce_disk_budget(Path("/tmp"), 5.0, "before campaign"),
                6 * 1024 ** 3)
            self.assertEqual(
                MODULE.enforce_disk_budget(Path("/tmp"), 0.0, "before campaign"),
                6 * 1024 ** 3)


if __name__ == "__main__":
    unittest.main()
