#!/usr/bin/env python3
"""Conservative Spec 144 cell and treatment analysis."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Iterable, Mapping


def nearest_rank(values: Iterable[float], percentile: float) -> float | None:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        return None
    if not 0.0 <= percentile <= 100.0:
        raise ValueError("percentile must be in [0, 100]")
    rank = max(1, math.ceil(percentile / 100.0 * len(ordered)))
    return ordered[rank - 1]


def latency_summary(values: Iterable[float]) -> dict:
    samples = [float(value) for value in values]
    if not samples:
        return {
            "count": 0, "meanMs": None, "p50Ms": None, "p95Ms": None,
            "p99Ms": None, "maxMs": None,
            "unavailableReason": "no-latency-observations",
        }
    return {
        "count": len(samples),
        "meanMs": sum(samples) / len(samples),
        "p50Ms": nearest_rank(samples, 50),
        "p95Ms": nearest_rank(samples, 95),
        "p99Ms": nearest_rank(samples, 99),
        "maxMs": max(samples),
    }


def ratio(numerator: int, denominator: int, reason: str) -> dict:
    return {
        "value": None if denominator == 0 else numerator / denominator,
        "numerator": int(numerator),
        "denominator": int(denominator),
        "unavailableReason": reason if denominator == 0 else "",
    }


def interest_utility(native: Mapping[str, object]) -> dict:
    total = int(native.get("payloadInterests", 0))
    useful = int(native.get("payloadApplicationUsefulInterests", 0))
    protection = int(native.get("payloadProtectionOnlyInterests", 0))
    nonproductive = int(native.get("payloadNonproductiveInterests", 0))
    unresolved = int(native.get("payloadUnresolvedInterests", 0))
    source = int(native.get("payloadSourceDataAdmissions", 0))
    repair_data = int(native.get("payloadRepairDataResponses", 0))
    repair_consumed = int(native.get("payloadRepairDataConsumed", 0))
    errors = []
    if useful != source + repair_consumed:
        errors.append("application-useful-does-not-match-source-plus-consumed-repair")
    if protection != repair_data - repair_consumed:
        errors.append("protection-only-does-not-match-unconsumed-repair")
    if repair_consumed > repair_data:
        errors.append("consumed-repair-exceeds-repair-data")
    if total != useful + protection + nonproductive + unresolved:
        errors.append("payload-interest-conservation-failed")
    if min(total, useful, protection, nonproductive, unresolved) < 0:
        errors.append("negative-interest-counter")
    return {
        "allPayloadInterests": total,
        "applicationUseful": useful,
        "protectionOnly": protection,
        "nonproductive": nonproductive,
        "unresolved": unresolved,
        "sourceAdmissions": source,
        "repairDataResponses": repair_data,
        "repairDataConsumed": repair_consumed,
        "nonproductiveInterestRatio": ratio(
            nonproductive, total, "no-payload-interests"),
        "protectionOnlyRatio": ratio(
            protection, total, "no-payload-interests"),
        "combinedNonApplicationInterestRatio": ratio(
            protection + nonproductive, total, "no-payload-interests"),
        "conserved": not errors,
        "errors": errors,
    }


def summarize_cell(provider_status: Mapping[str, object],
                   consumer_status: Mapping[str, object], *,
                   workload: str, profile: str) -> dict:
    if workload not in {"telemetry", "acoustic"}:
        raise ValueError("unsupported workload")
    provider_native = dict(provider_status.get("nativeStatus", {}))
    consumer_native = dict(consumer_status.get("nativeStatus", {}))
    expected = int(consumer_status.get("expectedMeasured", 0))
    complete = int(consumer_status.get("completeMeasured", 0))
    latencies = list(consumer_status.get("latencyMs", []))
    utility = interest_utility(consumer_native)
    payload_total = int(consumer_native.get("payloadInterests", 0))
    payload_source = int(consumer_native.get("payloadSourceInterests", 0))
    payload_repair = int(consumer_native.get("payloadRepairInterests", 0))
    payload_unclassified = int(
        consumer_native.get("payloadUnclassifiedInterests", 0))
    initial_source = int(
        consumer_native.get("initialPayloadSourceInterests", 0))
    retry_source = int(
        consumer_native.get("retryPayloadSourceInterests", 0))
    initial_repair = int(
        consumer_native.get("initialPayloadRepairInterests", 0))
    retry_repair = int(
        consumer_native.get("retryPayloadRepairInterests", 0))
    payload_kind_conserved = (
        payload_total == payload_source + payload_repair + payload_unclassified
        and payload_source == initial_source + retry_source
        and payload_repair == initial_repair + retry_repair
        and payload_unclassified == 0
    )
    mapping = ratio(
        int(consumer_native.get("mappingNewDataResponses", 0)),
        int(consumer_native.get("mappingDataResponses", 0)),
        "no-validated-mapping-data")
    future = ratio(
        int(provider_native.get("providerFutureHits", 0)),
        int(provider_native.get("providerFutureInterests", 0)),
        "no-provider-confirmed-future-interests")
    summary = {
        "schemaVersion": "spec144-uav-sensor-cell-analysis-v1",
        "workload": workload,
        "profile": profile,
        "expectedMeasured": expected,
        "completeMeasured": complete,
        "deliveryRatio": None if expected == 0 else complete / expected,
        "invalid": int(consumer_status.get("invalid", 0)),
        "duplicates": int(consumer_status.get("duplicates", 0)),
        "outOfOrder": int(consumer_status.get("outOfOrder", 0)),
        "monotonicStateViolations": int(
            consumer_status.get("monotonicStateViolations", 0)),
        "longestGapMs": float(consumer_status.get("longestGapMs", 0.0)),
        "latency": latency_summary(latencies),
        "clockDomain": consumer_status.get("clockDomain", ""),
        "latencyOrigin": consumer_status.get("latencyOrigin", ""),
        "latencyTerminal": consumer_status.get("latencyTerminal", ""),
        "mappingNovelty": mapping,
        "futureHit": future,
        "interestUtility": utility,
        "application": {
            "attemptedMeasured": int(
                provider_status.get("attemptedMeasured", 0)),
            "producedMeasured": int(
                provider_status.get("producedMeasured", 0)),
            "uniqueDeliveredMeasured": complete,
            "invalid": int(consumer_status.get("invalid", 0)),
            "duplicates": int(consumer_status.get("duplicates", 0)),
            "outOfOrder": int(consumer_status.get("outOfOrder", 0)),
            "terminalSkips": int(
                consumer_native.get("deadlineSkips", 0)),
        },
        "network": {
            "mappingInterests": int(consumer_native.get("mappingInterests", 0)),
            "mappingDataResponses": int(
                consumer_native.get("mappingDataResponses", 0)),
            "mappingNewDataResponses": int(
                consumer_native.get("mappingNewDataResponses", 0)),
            "payloadInterests": int(consumer_native.get("payloadInterests", 0)),
            "initialPayloadInterests": int(
                consumer_native.get("initialPayloadInterests", 0)),
            "retryPayloadInterests": int(
                consumer_native.get("retryPayloadInterests", 0)),
            "payloadSourceInterests": payload_source,
            "initialPayloadSourceInterests": initial_source,
            "retryPayloadSourceInterests": retry_source,
            "payloadRepairInterests": payload_repair,
            "initialPayloadRepairInterests": initial_repair,
            "retryPayloadRepairInterests": retry_repair,
            "payloadUnclassifiedInterests": payload_unclassified,
            "payloadKindConserved": payload_kind_conserved,
            "retryAttempts": int(consumer_native.get("retryAttempts", 0)),
            "retrySuccesses": int(consumer_native.get("retrySuccesses", 0)),
            "retrySuppressions": int(
                consumer_native.get("retrySuppressions", 0)),
            "retrySuppressionReasons": dict(
                consumer_native.get("retrySuppressionReasons", {})),
            "timeouts": int(consumer_native.get("timeouts", 0)),
            "nacks": int(consumer_native.get("nacks", 0)),
            "lateArrivals": int(consumer_native.get("lateArrivals", 0)),
            "deadlineSkips": int(consumer_native.get("deadlineSkips", 0)),
            "retryExhaustions": int(
                consumer_native.get("retryExhaustions", 0)),
            "recoveryAttempts": int(
                consumer_native.get("recoveryAttempts", 0)),
            "recoveryExhaustions": int(
                consumer_native.get("recoveryExhaustions", 0)),
            "providerFutureInterests": int(
                provider_native.get("providerFutureInterests", 0)),
            "providerFutureHits": int(
                provider_native.get("providerFutureHits", 0)),
            "providerInitialFutureInterests": int(
                provider_native.get("providerInitialFutureInterests", 0)),
            "providerInitialFutureHits": int(
                provider_native.get("providerInitialFutureHits", 0)),
            "providerRetryFutureInterests": int(
                provider_native.get("providerRetryFutureInterests", 0)),
            "providerRetryFutureHits": int(
                provider_native.get("providerRetryFutureHits", 0)),
        },
        "recovery": {
            "recoveredBlocks": int(consumer_status.get("recoveredBlocks", 0)),
            "recoveredSources": int(consumer_status.get("recoveredSources", 0)),
            "coreRecoveredSources": int(consumer_native.get("recovered", 0)),
            "declaredCapacity": int(
                consumer_native.get("declaredRecoveryCapacity", 0)),
            "successRate": ratio(
                int(consumer_native.get("recovered", 0)),
                int(consumer_native.get("recoveryAttempts", 0)),
                "no-recovery-attempts"),
        },
        "coreFetchDecision": consumer_native.get("fetchDecision"),
    }
    summary["gates"] = evaluate_cell(summary)
    summary["passed"] = all(summary["gates"].values())
    return summary


def evaluate_cell(summary: Mapping[str, object]) -> dict[str, bool]:
    workload = str(summary["workload"])
    profile = str(summary["profile"])
    zero = profile == "zero-loss"
    expected = int(summary["expectedMeasured"])
    complete = int(summary["completeMeasured"])
    latency = summary["latency"]
    utility = summary["interestUtility"]
    mapping = summary["mappingNovelty"]
    future = summary["futureHit"]
    delivery_requirement = 1.0 if zero else 0.999
    p95_limit = 100.0 if workload == "telemetry" and zero else \
        200.0 if workload == "telemetry" else \
        150.0 if zero else 250.0
    p99_limit = 150.0 if workload == "telemetry" and zero else \
        300.0 if workload == "telemetry" else \
        250.0 if zero else 400.0
    gap_limit = 100.0 if workload == "telemetry" and zero else \
        250.0 if workload == "telemetry" else \
        160.0 if zero else 320.0
    future_limit = 0.99 if workload == "telemetry" and zero else 0.95
    nonproductive_limit = 0.01 if zero else 0.10
    delivery_ratio = None if expected == 0 else complete / expected
    return {
        "identityAndScope": (
            expected in {1200, 1500}
            and summary.get("clockDomain") == "shared-host-steady-clock"
            and summary.get("latencyOrigin") == "source-or-capture-ready"
            and summary.get("latencyTerminal") ==
                "complete-application-admission"),
        "delivery": (
            delivery_ratio is not None
            and delivery_ratio >= delivery_requirement
            and (not zero or complete == expected)),
        "applicationAdmission": (
            int(summary.get("invalid", 0)) == 0
            and int(summary.get("duplicates", 0)) == 0
            and int(summary.get("monotonicStateViolations", 0)) == 0),
        "latency": (
            int(latency.get("count", 0)) == complete
            and latency.get("p95Ms") is not None
            and float(latency["p95Ms"]) <= p95_limit
            and float(latency["p99Ms"]) <= p99_limit),
        "longestGap": float(summary.get("longestGapMs", math.inf)) <= gap_limit,
        "mappingNovelty": (
            mapping.get("value") is not None
            and float(mapping["value"]) >= 0.99),
        "futureHit": (
            future.get("value") is not None
            and float(future["value"]) >= future_limit),
        "interestConservation": (
            bool(utility.get("conserved"))
            and int(utility.get("unresolved", -1)) == 0
            and bool(summary.get("network", {}).get(
                "payloadKindConserved", False))),
        "interestUtility": (
            utility["nonproductiveInterestRatio"].get("value") is not None
            and float(utility["nonproductiveInterestRatio"]["value"])
                <= nonproductive_limit),
    }


_EXACT_INTERVALS_N5 = {
    0: (0.0, 0.521824), 1: (0.005051, 0.716418),
    2: (0.052745, 0.853367), 3: (0.146633, 0.947255),
    4: (0.283582, 0.994949), 5: (0.478176, 1.0),
}


def exact_interval(accepted: int, attempted: int) -> tuple[float, float]:
    if attempted == 5 and accepted in _EXACT_INTERVALS_N5:
        return _EXACT_INTERVALS_N5[accepted]
    if attempted == 1 and accepted in {0, 1}:
        return (0.0, 0.975) if accepted == 0 else (0.025, 1.0)
    raise ValueError("only preregistered n=1/n=5 intervals are supported")


def aggregate(cells: Iterable[Mapping[str, object]]) -> dict:
    rows = list(cells)
    treatments = []
    workload_verdicts = {}
    for workload in ("telemetry", "acoustic"):
        workload_pass = True
        for profile in ("zero-loss", "loss", "reorder", "combined"):
            selected = [
                row for row in rows
                if row.get("workload") == workload and row.get("profile") == profile
            ]
            accepted = sum(bool(row.get("passed")) for row in selected)
            attempted = len(selected)
            required = 1 if profile == "zero-loss" else 4
            interval = exact_interval(accepted, attempted) if attempted in {1, 5} \
                else (None, None)
            passed = attempted == (1 if profile == "zero-loss" else 5) \
                and accepted >= required
            workload_pass = workload_pass and passed
            treatments.append({
                "workload": workload, "profile": profile,
                "accepted": accepted, "attempted": attempted,
                "required": required, "interval95": list(interval),
                "passed": passed,
            })
        workload_verdicts[workload] = workload_pass
    return {
        "schemaVersion": "spec144-uav-sensor-campaign-analysis-v1",
        "cells": len(rows),
        "treatments": treatments,
        "workloadVerdicts": workload_verdicts,
        "sharedGeneralityVerdict": (
            len(rows) == 32 and all(workload_verdicts.values())),
    }


def analyze_root(root: Path) -> dict:
    summaries = []
    for path in sorted((root / "cells").glob("*/summary.json")):
        value = json.loads(path.read_text(encoding="utf-8"))
        analysis = value.get("analysis", {})
        analysis["cellId"] = value.get("cellId", path.parent.name)
        summaries.append(analysis)
    campaign = aggregate(summaries)
    (root / "campaign-analysis.json").write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    with (root / "campaign-cells.csv").open("w", newline="", encoding="utf-8") as output:
        fields = [
            "cellId", "workload", "profile", "passed", "completeMeasured",
            "expectedMeasured", "deliveryRatio", "p50Ms", "p95Ms", "p99Ms",
            "longestGapMs", "futureHitRatio", "mappingNoveltyRatio",
            "nonproductiveInterestRatio", "protectionOnlyRatio",
            "timeouts", "nacks", "retryAttempts", "recoveredSources",
        ]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in summaries:
            writer.writerow({
                "cellId": row.get("cellId"),
                "workload": row.get("workload"),
                "profile": row.get("profile"),
                "passed": row.get("passed"),
                "completeMeasured": row.get("completeMeasured"),
                "expectedMeasured": row.get("expectedMeasured"),
                "deliveryRatio": row.get("deliveryRatio"),
                "p50Ms": row.get("latency", {}).get("p50Ms"),
                "p95Ms": row.get("latency", {}).get("p95Ms"),
                "p99Ms": row.get("latency", {}).get("p99Ms"),
                "longestGapMs": row.get("longestGapMs"),
                "futureHitRatio": row.get("futureHit", {}).get("value"),
                "mappingNoveltyRatio": row.get("mappingNovelty", {}).get("value"),
                "nonproductiveInterestRatio": row.get(
                    "interestUtility", {}).get(
                        "nonproductiveInterestRatio", {}).get("value"),
                "protectionOnlyRatio": row.get(
                    "interestUtility", {}).get(
                        "protectionOnlyRatio", {}).get("value"),
                "timeouts": row.get("network", {}).get("timeouts"),
                "nacks": row.get("network", {}).get("nacks"),
                "retryAttempts": row.get("network", {}).get("retryAttempts"),
                "recoveredSources": row.get("recovery", {}).get("recoveredSources"),
            })
    return campaign


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--spec", type=Path)
    args = parser.parse_args()
    result = analyze_root(args.input.resolve())
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
