#!/usr/bin/env python3
"""Deterministic contract tests for the Spec 126 MiniNDN impairment hook."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
RUNNER = REPO / "Experiments/NDNSF_UAV_GUI_Minindn.py"
CAMPAIGN = REPO / "Experiments/run_spec126_loss_reorder_matrix.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("spec126_uav_runner", RUNNER)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def load_campaign():
    spec = importlib.util.spec_from_file_location("spec126_campaign", CAMPAIGN)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def profile(**overrides):
    values = {
        "experiment_netem_loss_percent": 1.0,
        "experiment_netem_delay_ms": 20.0,
        "experiment_netem_jitter_ms": 10.0,
        "experiment_netem_reorder_percent": 25.0,
        "experiment_netem_reorder_correlation_percent": 50.0,
        "experiment_netem_reorder_gap": 5,
        "gs_node": "memphis",
        "drone_node": "ucla",
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class _Interface:
    def __init__(self, name: str):
        self.name = name


class _Node:
    def __init__(self, name: str):
        self.name = name
        self.commands: list[str] = []

    def intfList(self):
        return [_Interface("lo"), _Interface(f"{self.name}-eth0")]

    def cmd(self, command: str) -> str:
        self.commands.append(command)
        if command.startswith("tc -s qdisc"):
            return ("qdisc netem 10: parent 5:1 limit 1000 delay 20ms 10ms "
                    "loss 1% reorder 25% 50% gap 5\n")
        return "\n__RC__0"


class _Network:
    def __init__(self):
        self.net = {name: _Node(name) for name in ("memphis", "ucla")}


class Spec126LossReorderRunnerTest(unittest.TestCase):
    def test_frozen_matrix_has_one_plus_three_by_five_cells(self) -> None:
        campaign = load_campaign()
        cells = campaign.frozen_cells()
        self.assertEqual(len(cells), 16)
        self.assertEqual(cells[0]["id"], "zero-loss-run-01")
        self.assertEqual(cells[-1]["id"], "combined-run-05")
        self.assertEqual(
            {name: sum(cell["treatment"] == name for cell in cells)
             for name in ("zero-loss", "isolated-loss", "reorder", "combined")},
            {"zero-loss": 1, "isolated-loss": 5, "reorder": 5, "combined": 5},
        )

    def test_frozen_source_manifest_covers_runtime_binding_and_analyzer(self) -> None:
        campaign = load_campaign()
        names = {path.as_posix() for path in campaign.FROZEN_SOURCE_PATHS}
        self.assertIn("ndn-service-framework/Stream.cpp", names)
        self.assertIn("pythonWrapper/src/ndnsf/_ndnsf.cpp", names)
        self.assertIn("Experiments/run_spec126_loss_reorder_matrix.py", names)
        self.assertIn("Experiments/NDNSF_UAV_GUI_Minindn.py", names)
        self.assertTrue(all((campaign.ROOT / path).is_file()
                            for path in campaign.FROZEN_SOURCE_PATHS))

    def test_exact_interval_and_group_gate_preserve_failures(self) -> None:
        campaign = load_campaign()
        lower, upper = campaign.exact_interval(4, 5)
        self.assertLess(lower, 0.8)
        self.assertGreater(upper, 0.8)
        runs = [
            {"cell": {"treatment": treatment}, "accepted": accepted}
            for treatment, accepted in (
                [("zero-loss", True)] + [("isolated-loss", True)] * 4 +
                [("isolated-loss", False)] + [("reorder", True)] * 5 +
                [("combined", True)] * 3 + [("combined", False)] * 2)
        ]
        treatments = {row["treatment"]: row for row in campaign.aggregate(runs)}
        self.assertTrue(treatments["isolated-loss"]["passed"])
        self.assertFalse(treatments["combined"]["passed"])

    def test_traffic_metrics_separate_payload_mapping_and_failures(self) -> None:
        campaign = load_campaign()
        metrics = campaign.traffic_metrics(
            {
                "payload_interests": "202",
                "mapping_interests": "101",
                "mapping_data_responses": "100",
                "mapping_new_data_responses": "96",
                "retry_attempts": "7",
                "timeouts": "5",
                "nacks": "3",
            },
            {"provider_future_interests": "190", "provider_future_hits": "188"},
            fec_groups=100,
        )
        self.assertEqual(metrics["payloadInterests"], 202)
        self.assertEqual(metrics["mappingInterests"], 101)
        self.assertEqual(metrics["mappingDataResponses"], 100)
        self.assertEqual(metrics["mappingNewDataResponses"], 96)
        self.assertAlmostEqual(metrics["mappingNewDataRatio"], 0.96)
        self.assertAlmostEqual(metrics["payloadInterestOverheadRatio"], 0.01)
        self.assertEqual(metrics["retryAttempts"], 7)
        self.assertEqual(metrics["timeouts"], 5)
        self.assertEqual(metrics["nacks"], 3)

    def test_deadline_skip_is_not_misclassified_as_duplicate_delivery(self) -> None:
        campaign = load_campaign()
        self.assertTrue(campaign.no_duplicate_application_delivery({
            "duplicates": "0", "rejected": "38", "deadline_skips": "38",
        }))
        self.assertFalse(campaign.no_duplicate_application_delivery({
            "duplicates": "1", "rejected": "0", "deadline_skips": "0",
        }))

    def test_campaign_csv_exposes_separate_interest_metrics(self) -> None:
        campaign = load_campaign()
        output = io.StringIO()
        campaign.write_campaign_runs_csv(output, [{
            "cell": {"id": "zero-loss-run-01", "treatment": "zero-loss",
                     "repetition": 1},
            "returnCode": 0,
            "payloadInterests": 202,
            "mappingInterests": 101,
            "mappingDataResponses": 100,
            "mappingNewDataResponses": 96,
            "mappingNewDataRatio": 0.96,
            "retryAttempts": 7,
            "timeouts": 5,
            "nacks": 3,
            "accepted": True,
        }])
        rows = list(csv.DictReader(io.StringIO(output.getvalue())))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["payloadInterests"], "202")
        self.assertEqual(rows[0]["mappingInterests"], "101")
        self.assertEqual(rows[0]["mappingNewDataRatio"], "0.96")
        self.assertEqual(rows[0]["timeouts"], "5")

    def test_frozen_combined_profile_has_exact_ordering_terms(self) -> None:
        runner = load_runner()
        self.assertEqual(
            runner.build_experiment_netem_arguments(profile()),
            ["limit", "1000", "delay", "20ms", "10ms", "distribution",
             "normal", "loss", "random", "1%", "reorder", "25%", "50%",
             "gap", "5"],
        )

    def test_reordering_without_delay_or_gap_is_rejected(self) -> None:
        runner = load_runner()
        with self.assertRaises(ValueError):
            runner.build_experiment_netem_arguments(
                profile(experiment_netem_delay_ms=0.0))
        with self.assertRaises(ValueError):
            runner.build_experiment_netem_arguments(
                profile(experiment_netem_reorder_gap=0))

    def test_hook_applies_both_endpoints_and_persists_effective_qdisc(self) -> None:
        runner = load_runner()
        network = _Network()
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            runner.apply_experiment_netem(network, profile(), output)
            evidence = (output / "experiment-netem-before-apps.json").read_text(
                encoding="utf-8")

        self.assertIn('"phase": "before-nfd-and-apps"', evidence)
        self.assertIn('"reorder"', evidence)
        for node in network.net.values():
            installs = [command for command in node.commands
                        if command.startswith("tc qdisc replace")]
            self.assertEqual(len(installs), 1)
            self.assertIn("parent 5:1 handle 10: netem", installs[0])


if __name__ == "__main__":
    unittest.main()
