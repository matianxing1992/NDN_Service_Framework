#!/usr/bin/env python3
"""Runtime-aware NativeTracer campaign contract tests."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
SWEEP = REPO / "Experiments/NDNSF_DI_RuntimeAware_RpsSweep.py"
FIXTURE = (
    REPO /
    "examples/python/NDNSF-DistributedInference/native_di_tracer/runtime_aware_fixtures/multi_user_requests.json"
)


def load_harness_module():
    spec = importlib.util.spec_from_file_location("native_tracer_minindn", HARNESS)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class RuntimeAwareCampaignTest(unittest.TestCase):
    def test_dry_run_accepts_multi_user_runtime_aware_arguments(self) -> None:
        env = dict(os.environ)
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "Experiments"),
            env.get("PYTHONPATH", ""),
        ])
        completed = subprocess.run([
            sys.executable,
            str(HARNESS),
            "--dry-run",
            "--runtime-aware-user-planner",
            "--multi-user-workload", str(FIXTURE),
            "--runtime-aware-max-replans", "1",
            "--runtime-aware-replan-reasons", "FRAGMENT_EVICTED",
            "--requests", "1",
            "--concurrency", "1",
        ], cwd=str(REPO), env=env, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["event"], "NDNSF_DI_NATIVE_TRACER_MININDN_DRY_RUN")
        self.assertTrue(payload["runtimeAwareUserPlanner"])
        self.assertEqual(payload["multiUserWorkload"]["requestCount"], 2)
        self.assertEqual(payload["requests"], 2)
        self.assertEqual(payload["runtimeAwareMaxReplans"], 1)
        self.assertIn("--runtime-aware-max-replans 1", payload["userDriverCommand"])
        self.assertIn("FRAGMENT_EVICTED", payload["userDriverCommand"])

    def test_planner_metrics_aggregation_reports_campaign_fields(self) -> None:
        harness = load_harness_module()
        metrics = harness.build_campaign_metrics({
            "status": "SUCCESS",
            "requestCount": 3,
            "userExecution": {
                "requestCount": 3,
                "successCount": 2,
                "failureCount": 1,
                "p50Ms": 10.0,
                "p95Ms": 25.0,
                "meanMs": 14.0,
                "makespanMs": 42.0,
                "replanCount": 1,
            },
            "providerUtilization": {
                "/P1": {"estimatedUtilization": 0.5},
                "/P2": {"estimatedUtilization": 0.25},
            },
            "failureBreakdown": {
                "negativeAckEventCount": 1,
            },
            "runtimeAwarePlanner": {
                "selectedProviders": {
                    "/Backbone": "/P1",
                    "/Merge": "/P2",
                },
                "selectedResidencies": {
                    "/Backbone": "GPU_LOADED",
                    "/Merge": "CPU_RESIDENT",
                },
                "nodeCostSummary": {"totalMs": 3.0},
                "edgeCostSummary": {"totalMs": 4.0},
            },
            "leaseCounters": {
                "granted": 3,
                "consumed": 2,
                "expired": 1,
            },
            "providerFragmentInventory": {
                "residencyCounters": {
                    "CPU_RESIDENT": 4,
                    "DISK_RESIDENT": 4,
                },
            },
            "rpsSweep": [
                {"targetRps": 4, "status": "SUCCESS", "successRate": 1.0, "p95Ms": 20.0},
                {"targetRps": 8, "status": "SUCCESS", "successRate": 0.995, "p95Ms": 35.0},
                {"targetRps": 12, "status": "FAILURE", "successRate": 0.90, "failureRate": 0.10},
            ],
        })
        self.assertEqual(metrics["requestCount"], 3)
        self.assertEqual(metrics["successRate"], round(2 / 3, 6))
        self.assertEqual(metrics["latencyMs"]["p95"], 25.0)
        self.assertEqual(metrics["leaseCounters"]["negativeAckEvents"], 1)
        self.assertEqual(metrics["leaseCounters"]["granted"], 3)
        self.assertEqual(metrics["leaseCounters"]["consumed"], 2)
        self.assertEqual(metrics["residencyCounters"]["GPU_LOADED"], 1)
        self.assertEqual(metrics["residencyCounters"]["CPU_RESIDENT"], 1)
        self.assertEqual(metrics["observedResidencyCounters"]["CPU_RESIDENT"], 4)
        self.assertEqual(metrics["observedResidencyCounters"]["DISK_RESIDENT"], 4)
        self.assertEqual(metrics["maxStableRps"], 8.0)
        self.assertEqual(metrics["utilization"]["meanEstimatedUtilization"], 0.375)
        self.assertEqual(metrics["replanCount"], 1)

    def test_admission_lease_log_counters_are_aggregated(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "provider.log"
            log.write_text(
                "NDNSF_ADMISSION_LEASE_ACCEPTED provider=/P requester=/U "
                "service=/S requestId=/r leaseId=l1\n"
                "NDNSF_ADMISSION_LEASE_REJECTED provider=/P requester=/U "
                "service=/S requestId=/r leaseId=l2 reason=LEASE_EXPIRED\n",
                encoding="utf-8")
            counters = harness.collect_admission_lease_counters(Path(tmp))
        self.assertEqual(counters["consumed"], 1)
        self.assertEqual(counters["rejected"], 1)
        self.assertEqual(counters["expired"], 1)
        self.assertEqual(counters["reasons"]["LEASE_EXPIRED"], 1)

    def test_provider_fragment_inventory_events_are_aggregated(self) -> None:
        harness = load_harness_module()
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "provider.log"
            log.write_text(
                "NDNSF_DI_FRAGMENT_INVENTORY event=CPU_RESIDENT "
                "provider=unknown role=/Backbone fragmentDigest=sha256:bb "
                "backend=onnx-cpu path=/tmp/backbone.onnx residency=CPU_RESIDENT "
                "epoch_ms=1\n"
                "NDNSF_DI_FRAGMENT_INVENTORY event=EXECUTION_OBSERVED "
                "provider=/P1 role=/Backbone fragmentDigest=sha256:bb "
                "backend=onnx-cpu path=/tmp/backbone.onnx residency=CPU_RESIDENT "
                "epoch_ms=2\n"
                "NDNSF_DI_FRAGMENT_INVENTORY event=EVICTED "
                "provider=unknown role=/Backbone fragmentDigest=sha256:bb "
                "backend=onnx-cpu path=/tmp/backbone.onnx residency=DISK_RESIDENT "
                "epoch_ms=3\n",
                encoding="utf-8")
            inventory = harness.collect_provider_fragment_inventory(Path(tmp))

        self.assertEqual(inventory["eventCount"], 3)
        self.assertEqual(inventory["eventCounters"]["CPU_RESIDENT"], 1)
        self.assertEqual(inventory["eventCounters"]["EXECUTION_OBSERVED"], 1)
        self.assertEqual(inventory["eventCounters"]["EVICTED"], 1)
        self.assertEqual(inventory["residencyCounters"]["CPU_RESIDENT"], 2)
        self.assertEqual(inventory["residencyCounters"]["DISK_RESIDENT"], 1)
        latest = inventory["latestByProviderRole"]["/P1|/Backbone"]
        self.assertEqual(latest["fragmentDigest"], "sha256:bb")
        self.assertEqual(latest["residency"], "CPU_RESIDENT")

    def test_rps_sweep_dry_run_builds_runtime_aware_commands(self) -> None:
        completed = subprocess.run([
            sys.executable,
            str(SWEEP),
            "--dry-run",
            "--out", "/tmp/ndnsf-di-rps-sweep-dry-run",
            "--rps", "0.2,0.4",
            "--requests", "2",
            "--concurrency", "2",
            "--",
            "--provider-check-timeout", "60",
        ], cwd=str(REPO), text=True, stdout=subprocess.PIPE,
           stderr=subprocess.PIPE, check=True)
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["status"], "DRY_RUN")
        self.assertEqual(len(payload["commands"]), 2)
        first = " ".join(payload["commands"][0])
        self.assertIn("--runtime-aware-user-planner", first)
        self.assertIn("--target-rps 0.2", first)
        self.assertIn("--provider-check-timeout 60", first)


if __name__ == "__main__":
    unittest.main()
