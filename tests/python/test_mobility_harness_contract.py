#!/usr/bin/env python3

import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
EXPERIMENTS = REPO_ROOT / "Experiments"
sys.path.insert(0, str(EXPERIMENTS))
SPEC = importlib.util.spec_from_file_location(
    "wifi_mobility_harness", EXPERIMENTS / "WifiRouterMobilityReliability.py")
mobility = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mobility)


class MobilityHarnessReceiptContractTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.run_dir = self.root / "cells" / "range-200-grpc"
        self.run_dir.mkdir(parents=True)
        self.source_hashes = [{
            "kind": "source", "path": str(EXPERIMENTS / "dummy.py"),
            "sha256": "1" * 64,
        }]
        command = f"python3 {EXPERIMENTS / 'WifiRouterMobilityReliability.py'} --single-run"
        self.cell = {
            "cell_id": "range-200-grpc",
            "system": "grpc",
            "system_id": "grpc",
            "range_m": 200.0,
            "output_dir": str(self.run_dir),
            "trace_path": str(self.root / "traces" / "range_200.csv"),
            "trace_sha256": "2" * 64,
            "argv": ["python3", str(EXPERIMENTS / "WifiRouterMobilityReliability.py"),
                     "--single-run"],
            "command": command,
            "command_sha256": mobility.sha256_text(command),
            "source_hashes": self.source_hashes,
        }
        self.manifest = {
            "schema": mobility.CAMPAIGN_SCHEMA,
            "campaign_id": "mobility-test-campaign",
            "output_dir": str(self.root),
            "formal_baselines": False,
            "smoke": False,
            "source_hashes": self.source_hashes,
            "configuration": {
                "duration_s": 1,
                "rate_rps": 1.0,
                "traffic_phase_tolerance_s": 0.05,
            },
            "cells": [self.cell],
        }
        self.cell_manifest = {
            "schema": "ndnsf-mobility-baseline-cell-v1",
            "campaign_id": self.manifest["campaign_id"],
            **self.cell,
        }
        mobility.write_json(self.run_dir / "cell-manifest.json", self.cell_manifest)
        mobility.write_json(self.run_dir / "summary.json", {"summaries": []})
        mobility.write_json(self.run_dir / "runtime-commands.json", {"client": "test"})
        (self.run_dir / "mobility_trace.csv").write_text("time_s,provider\n")
        for name in (
                "grpc-client", "grpc-stats", "grpc-server-ucla",
                "grpc-server-wustl", "grpc-server-uiuc"):
            (self.run_dir / f"{name}.log").write_text(f"{name}\n")

        self.snapshot = self._stats_snapshot()
        stats_path = self.run_dir / "grpc-server-stats.json"
        mobility.write_json(stats_path, self.snapshot)
        for provider_id, snapshot in self.snapshot["providers"].items():
            mobility.write_json(
                self.run_dir / f"grpc-provider-stats-{provider_id}.json", snapshot)
        exact = mobility.summarize_grpc_server_stats(
            self.snapshot, sent=1, measurement_start_s=10.0, duration_s=1.0)
        runtime_path = self.run_dir / "runtime-commands.json"
        self.result = {
            "cell_id": self.cell["cell_id"],
            "campaign_id": self.manifest["campaign_id"],
            "system_id": "grpc",
            "system_label": "gRPC-HC-3",
            "range_m": 200.0,
            "output_dir": str(self.run_dir),
            "command": self.cell["command"],
            "command_sha256": self.cell["command_sha256"],
            "trace_sha256": self.cell["trace_sha256"],
            "trace_source": self.cell["trace_path"],
            "status": "passed",
            "summary_marker": "GRPC_FAILOVER_RATE",
            "sent": 1,
            "success": 1,
            "failures": 0,
            "attempts": 1,
            "failovers": 0,
            "success_rate": 100.0,
            "actual_rps": 1.0,
            "mean_ms": 5.0,
            "p50_ms": 5.0,
            "p95_ms": 5.0,
            "p99_ms": 5.0,
            "measurement_start_lateness_ms": 0.0,
            "measurement_start_monotonic_s": 10.0,
            "application_rpc_calls": 1,
            "application_messages": 2,
            "health_checks": 0,
            "health_success": 0,
            "handler_executions_observed": 1,
            "runtime_commands_file": str(runtime_path),
            "runtime_commands_sha256": mobility.sha256_file(runtime_path),
            "server_stats_file": str(stats_path),
            "server_stats_sha256": mobility.sha256_file(stats_path),
            **exact,
        }
        self.run = {
            "cell_id": self.cell["cell_id"],
            "campaign_id": self.manifest["campaign_id"],
            "system_id": "grpc",
            "system_label": "gRPC-HC-3",
            "range_m": 200.0,
            "status": "passed",
            "returncode": 0,
            "command": self.cell["command"],
            "command_sha256": self.cell["command_sha256"],
            "trace_sha256": self.cell["trace_sha256"],
            "output_dir": str(self.run_dir),
        }
        self._refresh_evidence()

    def tearDown(self):
        self.temporary.cleanup()

    def test_ndnsf_zero_provider_executions_are_valid_accounting(self):
        ndnsf_dir = self.root / "zero-execution-ndnsf"
        ndnsf_dir.mkdir()
        (ndnsf_dir / "ndnsf-provider-A.log").write_text(
            "INTERMITTENT_PROVIDER_ACK_SUPPRESSED provider=A service=/HELLO epoch=1\n")
        summary = mobility.summarize_ndnsf_provider_executions(ndnsf_dir)
        self.assertEqual(summary["attempts"], 0)
        self.assertFalse(summary["provider_execution_markers_present"])

    @staticmethod
    def _empty_provider(provider_id, snapshot_s=19.0):
        return {
            "schema": "ndnsf.grpc.baseline.provider-stats.v1",
            "provider_id": provider_id,
            "stats_epoch": 0,
            "stats_reset_monotonic_s": 9.0,
            "snapshot_monotonic_s": snapshot_s,
            "handler_executions": 0,
            "health_checks": 0,
            "health_success": 0,
            "request_id_counts": {},
            "service_status_counts": {},
            "health_status_counts": {},
            "service_events": [],
            "health_events": [],
        }

    def _stats_snapshot(self):
        providers = {
            provider: self._empty_provider(provider)
            for provider in mobility.provider_nodes()
        }
        providers["ucla"].update({
            "handler_executions": 1,
            "request_id_counts": {"0": 1},
            "service_status_counts": {"OK": 1},
            "service_events": [{
                "event_id": 0,
                "stats_epoch": 0,
                "request_id": "0",
                "request_name": "Test-0",
                "attempt": 1,
                "started_monotonic_s": 10.1,
                "completed_monotonic_s": 10.2,
                "status": "OK",
            }],
        })
        return {
            "schema": "ndnsf.grpc.baseline.aggregate-stats.v1",
            "snapshot_monotonic_s": 20.0,
            "providers": providers,
        }

    def _refresh_evidence(self):
        path = mobility.collect_cell_evidence(
            self.run_dir, self.cell, self.manifest)
        self.result["evidence_manifest_file"] = str(path)
        self.result["evidence_manifest_sha256"] = mobility.sha256_file(path)
        self.receipt = {
            "manifest": copy.deepcopy(self.cell_manifest),
            "run": copy.deepcopy(self.run),
            "result": copy.deepcopy(self.result),
        }

    def _rewrite_evidence(self, evidence):
        path = self.run_dir / "evidence-hashes.json"
        mobility.write_json(path, evidence)
        self.receipt["result"]["evidence_manifest_sha256"] = mobility.sha256_file(path)

    def test_exact_passed_receipt_is_accepted(self):
        run, result = mobility.validate_terminal_receipt(
            self.cell, self.receipt, self.manifest, self.run_dir)
        self.assertEqual(run["status"], "passed")
        self.assertEqual(result["server_handler_executions_snapshot_exact"], 1)

    def test_result_identity_mutations_are_rejected(self):
        mutations = {
            "cell_id": "other-cell",
            "range_m": 100.0,
            "output_dir": str(self.root / "other"),
            "command": "different command",
            "command_sha256": "a" * 64,
            "trace_sha256": "b" * 64,
        }
        for key, value in mutations.items():
            with self.subTest(key=key):
                receipt = copy.deepcopy(self.receipt)
                receipt["result"][key] = value
                with self.assertRaises(RuntimeError):
                    mobility.validate_terminal_receipt(
                        self.cell, receipt, self.manifest, self.run_dir)

    def test_run_is_required_and_exactly_bound(self):
        for key, value in (
                ("cell_id", "other"), ("range_m", 100.0),
                ("output_dir", str(self.root / "other")),
                ("command_sha256", "a" * 64),
                ("trace_sha256", "b" * 64), ("status", "failed")):
            with self.subTest(key=key):
                receipt = copy.deepcopy(self.receipt)
                receipt["run"][key] = value
                with self.assertRaises(RuntimeError):
                    mobility.validate_terminal_receipt(
                        self.cell, receipt, self.manifest, self.run_dir)
        receipt = copy.deepcopy(self.receipt)
        del receipt["run"]
        with self.assertRaises(RuntimeError):
            mobility.validate_terminal_receipt(
                self.cell, receipt, self.manifest, self.run_dir)

    def test_failed_and_interrupted_receipts_still_require_identity(self):
        for status, returncode in (("failed", 1), ("interrupted", -2)):
            with self.subTest(status=status):
                receipt = copy.deepcopy(self.receipt)
                receipt["result"]["status"] = status
                receipt["run"]["status"] = status
                receipt["run"]["returncode"] = returncode
                receipt["result"]["cell_id"] = "wrong"
                with self.assertRaises(RuntimeError):
                    mobility.validate_terminal_receipt(
                        self.cell, receipt, self.manifest, self.run_dir)

    def test_exact_failed_and_interrupted_receipts_are_accepted(self):
        for status, returncode in (("failed", 1), ("interrupted", -2)):
            with self.subTest(status=status):
                receipt = copy.deepcopy(self.receipt)
                receipt["result"]["status"] = status
                receipt["result"]["error"] = f"synthetic {status} terminal state"
                receipt["run"]["status"] = status
                receipt["run"]["returncode"] = returncode
                run, result = mobility.validate_terminal_receipt(
                    self.cell, receipt, self.manifest, self.run_dir)
                self.assertEqual(run["status"], status)
                self.assertEqual(result["status"], status)

    def test_empty_duplicate_and_incomplete_evidence_are_rejected(self):
        evidence_path = self.run_dir / "evidence-hashes.json"
        original = json.loads(evidence_path.read_text())
        cases = []
        empty = copy.deepcopy(original)
        empty["files"] = []
        cases.append(empty)
        duplicate = copy.deepcopy(original)
        duplicate["files"].append(copy.deepcopy(duplicate["files"][0]))
        cases.append(duplicate)
        incomplete = copy.deepcopy(original)
        incomplete["files"] = [
            entry for entry in incomplete["files"]
            if entry["path"] != "grpc-server-stats.json"]
        cases.append(incomplete)
        for index, evidence in enumerate(cases):
            with self.subTest(case=index):
                self._rewrite_evidence(evidence)
                with self.assertRaises(RuntimeError):
                    mobility.validate_terminal_receipt(
                        self.cell, self.receipt, self.manifest, self.run_dir)
        mobility.write_json(evidence_path, original)

    def test_unrelated_or_result_inconsistent_stats_are_rejected(self):
        stats_path = self.run_dir / "grpc-server-stats.json"
        mobility.write_json(stats_path, {
            "schema": "ndnsf.grpc.baseline.aggregate-stats.v1",
            "snapshot_monotonic_s": 20.0,
            "providers": {},
        })
        self.receipt["result"]["server_stats_sha256"] = mobility.sha256_file(stats_path)
        self._refresh_evidence()
        with self.assertRaises(RuntimeError):
            mobility.validate_terminal_receipt(
                self.cell, self.receipt, self.manifest, self.run_dir)

    def test_stats_path_and_derived_counts_are_bound(self):
        for key, value in (
                ("server_stats_file", str(self.root / "other-stats.json")),
                ("server_handler_executions_snapshot_exact", 0),
                ("server_request_id_execution_counts", {})):
            with self.subTest(key=key):
                receipt = copy.deepcopy(self.receipt)
                receipt["result"][key] = value
                with self.assertRaises(RuntimeError):
                    mobility.validate_terminal_receipt(
                        self.cell, receipt, self.manifest, self.run_dir)


class MobilityHarnessNumericContractTests(unittest.TestCase):
    def test_measurement_lateness_accepts_signed_finite_values(self):
        self.assertEqual(
            mobility.required_finite_number(
                {"measurement_start_lateness_ms": "-76.525"},
                "measurement_start_lateness_ms"),
            -76.525)
        with self.assertRaises(RuntimeError):
            mobility.required_finite_number(
                {"measurement_start_lateness_ms": "nan"},
                "measurement_start_lateness_ms")


class MobilityHarnessCampaignPlanTests(unittest.TestCase):
    def test_explicit_ndnsf_cell_replays_the_campaign_trace(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary).resolve()
            args = SimpleNamespace(
                duration_s=1, rate_rps=1.0, processing_delay_ms=5,
                timeout_ms=5000, ack_timeout_ms=1000,
                attempt_timeout_ms=1000, health_interval_ms=1000,
                traffic_start_delay_s=1.0, settle_seconds=0,
                trace_profile="random-waypoint", handoff_period_s=1.0,
                mobility_warmup_s=300.0,
                ndnsf_strategy="first-responding", seed=61,
                profile="four-provider-single-ap", provider_scope="",
                ap_layout="", speed_mps=2.0,
                grpc_no_health_routing=True, grpc_parallel=False,
                block_network=True, formal_baselines=False, smoke=False,
                include_ndnsf=False, ndnsf_response_retry=True,
                ndnsf_response_fault_provider="",
                ndnsf_response_fault_delay_ms=0,
                ndnsf_standby_ack_delay_ms=0,
            )

            def fake_trace(path, *_args, **_kwargs):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("time_s,provider,x,y,distance_m,in_range\n")
                return {"path": str(path.resolve()),
                        "sha256": mobility.sha256_file(path)}

            with mock.patch.object(
                    mobility, "generate_mobility_trace", side_effect=fake_trace), \
                    mock.patch.object(mobility, "runtime_provenance", return_value={}):
                manifest = mobility.build_campaign_plan(
                    args, output_dir, [100.0], ["ndnsf"], [])

            cell = manifest["cells"][0]
            self.assertEqual(cell["trace_path"], manifest["traces"][0]["path"])
            self.assertIn("--trace-replay", cell["argv"])
            replay_index = cell["argv"].index("--trace-replay")
            self.assertEqual(cell["argv"][replay_index + 1], cell["trace_path"])
            self.assertIn("--mobility-warmup-s", cell["argv"])
            self.assertEqual(
                cell["argv"][cell["argv"].index("--mobility-warmup-s") + 1],
                "300.0")
            self.assertEqual(manifest["configuration"]["mobility_warmup_s"], 300.0)


if __name__ == "__main__":
    unittest.main()
