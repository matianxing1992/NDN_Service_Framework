#!/usr/bin/env python3
"""Runtime-aware NativeTracer campaign contract tests."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
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
                "nodeCostSummary": {"totalMs": 3.0},
                "edgeCostSummary": {"totalMs": 4.0},
            },
        })
        self.assertEqual(metrics["requestCount"], 3)
        self.assertEqual(metrics["successRate"], round(2 / 3, 6))
        self.assertEqual(metrics["latencyMs"]["p95"], 25.0)
        self.assertEqual(metrics["leaseCounters"]["negativeAckEvents"], 1)
        self.assertEqual(metrics["utilization"]["meanEstimatedUtilization"], 0.375)
        self.assertEqual(metrics["replanCount"], 1)


if __name__ == "__main__":
    unittest.main()
