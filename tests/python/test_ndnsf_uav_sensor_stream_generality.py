#!/usr/bin/env python3
"""Deterministic metric and neutrality gates for Spec 144."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = ROOT / "Experiments/analyze_spec144_uav_sensor_stream.py"
SPEC = importlib.util.spec_from_file_location("spec144_analyzer", ANALYZER_PATH)
analyzer = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(analyzer)


def native(**overrides):
    value = {
        "payloadInterests": 100,
        "payloadSourceDataAdmissions": 80,
        "payloadRepairDataResponses": 10,
        "payloadRepairDataConsumed": 5,
        "payloadApplicationUsefulInterests": 85,
        "payloadProtectionOnlyInterests": 5,
        "payloadNonproductiveInterests": 10,
        "payloadUnresolvedInterests": 0,
        "mappingInterests": 10,
        "mappingDataResponses": 10,
        "mappingNewDataResponses": 10,
        "initialPayloadInterests": 90,
        "retryPayloadInterests": 10,
        "retryAttempts": 10,
        "retrySuccesses": 5,
        "retrySuppressions": 0,
        "timeouts": 5,
        "nacks": 5,
        "lateArrivals": 0,
        "deadlineSkips": 0,
        "retryExhaustions": 0,
        "recoveryAttempts": 5,
        "recoveryExhaustions": 0,
        "recovered": 5,
        "declaredRecoveryCapacity": 2,
    }
    value.update(overrides)
    repair_interests = int(value["payloadRepairDataResponses"])
    source_interests = int(value["payloadInterests"]) - repair_interests
    retry_source = min(int(value["retryPayloadInterests"]), source_interests)
    value.setdefault("payloadSourceInterests", source_interests)
    value.setdefault("initialPayloadSourceInterests",
                     source_interests - retry_source)
    value.setdefault("retryPayloadSourceInterests", retry_source)
    value.setdefault("payloadRepairInterests", repair_interests)
    value.setdefault("initialPayloadRepairInterests", repair_interests)
    value.setdefault("retryPayloadRepairInterests", 0)
    value.setdefault("payloadUnclassifiedInterests", 0)
    value.setdefault("retrySuppressionReasons", {})
    return value


class MetricContractTest(unittest.TestCase):
    def test_nearest_rank_and_mean_are_not_interpolated(self) -> None:
        values = [1, 2, 3, 4, 100]
        summary = analyzer.latency_summary(values)
        self.assertEqual(summary["meanMs"], 22)
        self.assertEqual(summary["p50Ms"], 3)
        self.assertEqual(summary["p95Ms"], 100)
        self.assertEqual(summary["p99Ms"], 100)

    def test_three_way_payload_interest_conservation(self) -> None:
        result = analyzer.interest_utility(native())
        self.assertTrue(result["conserved"])
        self.assertEqual(result["applicationUseful"], 85)
        self.assertEqual(result["protectionOnly"], 5)
        self.assertEqual(result["nonproductive"], 10)
        self.assertEqual(result["unresolved"], 0)

    def test_unresolved_and_double_classification_fail_closed(self) -> None:
        unresolved = analyzer.interest_utility(native(
            payloadNonproductiveInterests=9, payloadUnresolvedInterests=1))
        self.assertTrue(unresolved["conserved"])
        self.assertEqual(unresolved["unresolved"], 1)
        broken = analyzer.interest_utility(native(
            payloadApplicationUsefulInterests=86))
        self.assertFalse(broken["conserved"])
        self.assertIn(
            "application-useful-does-not-match-source-plus-consumed-repair",
            broken["errors"])

    def test_payload_source_repair_split_must_conserve(self) -> None:
        provider = {"nativeStatus": {
            "providerFutureInterests": 100,
            "providerFutureHits": 100,
        }}
        consumer = {
            "expectedMeasured": 1200, "completeMeasured": 1200,
            "latencyMs": [10.0] * 1200, "longestGapMs": 50.0,
            "clockDomain": "shared-host-steady-clock",
            "latencyOrigin": "source-or-capture-ready",
            "latencyTerminal": "complete-application-admission",
            "invalid": 0, "duplicates": 0, "outOfOrder": 0,
            "nativeStatus": native(payloadUnclassifiedInterests=1),
        }
        summary = analyzer.summarize_cell(
            provider, consumer, workload="telemetry", profile="zero-loss")
        self.assertFalse(summary["network"]["payloadKindConserved"])
        self.assertFalse(summary["gates"]["interestConservation"])
        self.assertFalse(summary["passed"])

    def test_mapping_and_future_ratios_use_separate_denominators(self) -> None:
        provider = {"nativeStatus": {
            "providerFutureInterests": 100,
            "providerFutureHits": 99,
        }}
        consumer = {
            "expectedMeasured": 1200, "completeMeasured": 1200,
            "latencyMs": [10.0] * 1200, "longestGapMs": 50.0,
            "clockDomain": "shared-host-steady-clock",
            "latencyOrigin": "source-or-capture-ready",
            "latencyTerminal": "complete-application-admission",
            "invalid": 0, "duplicates": 0, "outOfOrder": 0,
            "nativeStatus": native(
                payloadInterests=1200,
                payloadSourceDataAdmissions=1200,
                payloadRepairDataResponses=0,
                payloadRepairDataConsumed=0,
                payloadApplicationUsefulInterests=1200,
                payloadProtectionOnlyInterests=0,
                payloadNonproductiveInterests=0,
                mappingDataResponses=100,
                mappingNewDataResponses=99,
            ),
        }
        summary = analyzer.summarize_cell(
            provider, consumer, workload="telemetry", profile="zero-loss")
        self.assertEqual(summary["mappingNovelty"]["value"], 0.99)
        self.assertEqual(summary["futureHit"]["value"], 0.99)
        self.assertTrue(summary["passed"])

    def test_clock_domain_and_missing_latency_fail_gate(self) -> None:
        summary = {
            "workload": "telemetry", "profile": "zero-loss",
            "expectedMeasured": 1200, "completeMeasured": 1200,
            "clockDomain": "wall-clock", "latencyOrigin": "",
            "latencyTerminal": "", "invalid": 0, "duplicates": 0,
            "outOfOrder": 0, "latency": analyzer.latency_summary([]),
            "longestGapMs": 0,
            "mappingNovelty": {"value": 1.0},
            "futureHit": {"value": 1.0},
            "interestUtility": analyzer.interest_utility(native()),
        }
        gates = analyzer.evaluate_cell(summary)
        self.assertFalse(gates["identityAndScope"])
        self.assertFalse(gates["latency"])

    def test_exact_preregistered_intervals(self) -> None:
        self.assertEqual(analyzer.exact_interval(4, 5),
                         (0.283582, 0.994949))
        self.assertEqual(analyzer.exact_interval(1, 1), (0.025, 1.0))


class NeutralityContractTest(unittest.TestCase):
    def test_core_has_no_new_application_selectors(self) -> None:
        # Application words may exist in comments predating Spec 144; this gate
        # targets executable comparisons/branches, not documentation prose.
        for relative in (
                "ndn-service-framework/Stream.cpp",
                "ndn-service-framework/Stream.hpp",
                "pythonWrapper/src/ndnsf/_ndnsf.cpp"):
            text = (ROOT / relative).read_text(encoding="utf-8").lower()
            for token in (
                    '== "telemetry"', '== "audio"', '== "acoustic"',
                    '== "codec"', '== "uav"', 'workloadid'):
                self.assertNotIn(token, text, f"{token} in {relative}")


if __name__ == "__main__":
    unittest.main()
