#!/usr/bin/env python3
"""Conservative analysis for the one-shot Spec 146 acoustic matrix."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Iterable, Mapping

from analyze_spec144_uav_sensor_stream import exact_interval


PROFILES = ("zero-loss", "loss", "reorder", "combined")


def aggregate(cells: Iterable[Mapping[str, object]]) -> dict:
    rows = list(cells)
    treatments = []
    overall = len(rows) == 16
    for profile in PROFILES:
        selected = [
            row for row in rows
            if row.get("workload") == "acoustic" and row.get("profile") == profile
        ]
        accepted = sum(bool(row.get("passed")) for row in selected)
        attempted = len(selected)
        expected = 1 if profile == "zero-loss" else 5
        required = 1 if profile == "zero-loss" else 4
        interval = exact_interval(accepted, attempted) if attempted in {1, 5} \
            else (None, None)
        passed = attempted == expected and accepted >= required
        overall = overall and passed
        treatments.append({
            "workload": "acoustic",
            "profile": profile,
            "accepted": accepted,
            "attempted": attempted,
            "required": required,
            "interval95": list(interval),
            "passed": passed,
        })
    return {
        "schemaVersion": "spec146-acoustic-campaign-analysis-v1",
        "cells": len(rows),
        "treatments": treatments,
        "acousticStabilityVerdict": overall,
    }


def analyze_root(root: Path) -> dict:
    summaries = []
    for path in sorted((root / "cells").glob("*/summary.json")):
        value = json.loads(path.read_text(encoding="utf-8"))
        analysis = dict(value.get("analysis", {}))
        analysis["cellId"] = value.get("cellId", path.parent.name)
        summaries.append(analysis)
    campaign = aggregate(summaries)
    (root / "campaign-analysis.json").write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    fields = [
        "cellId", "profile", "passed", "completeMeasured", "expectedMeasured",
        "deliveryRatio", "meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs",
        "longestGapMs", "futureHitRatio", "mappingNoveltyRatio",
        "mappingInterests", "mappingDataResponses", "payloadInterests",
        "payloadSourceInterests", "payloadRepairInterests",
        "retryPayloadInterests", "timeouts", "nacks", "lateArrivals",
        "recoveryEligibleSources", "terminalMissingSources",
        "recoverableGroups", "recoveredSources",
        "recoveredGroups", "recoveryAttempts", "recoveryExhaustions",
        "nonproductiveInterestRatio", "protectionOnlyRatio",
        "interestConserved",
    ]
    with (root / "campaign-cells.csv").open(
            "w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in summaries:
            latency = row.get("latency", {})
            network = row.get("network", {})
            recovery = row.get("recovery", {})
            utility = row.get("interestUtility", {})
            writer.writerow({
                "cellId": row.get("cellId"),
                "profile": row.get("profile"),
                "passed": row.get("passed"),
                "completeMeasured": row.get("completeMeasured"),
                "expectedMeasured": row.get("expectedMeasured"),
                "deliveryRatio": row.get("deliveryRatio"),
                "meanMs": latency.get("meanMs"),
                "p50Ms": latency.get("p50Ms"),
                "p95Ms": latency.get("p95Ms"),
                "p99Ms": latency.get("p99Ms"),
                "maxMs": latency.get("maxMs"),
                "longestGapMs": row.get("longestGapMs"),
                "futureHitRatio": row.get("futureHit", {}).get("value"),
                "mappingNoveltyRatio": row.get(
                    "mappingNovelty", {}).get("value"),
                "mappingInterests": network.get("mappingInterests"),
                "mappingDataResponses": network.get("mappingDataResponses"),
                "payloadInterests": network.get("payloadInterests"),
                "payloadSourceInterests": network.get("payloadSourceInterests"),
                "payloadRepairInterests": network.get("payloadRepairInterests"),
                "retryPayloadInterests": network.get("retryPayloadInterests"),
                "timeouts": network.get("timeouts"),
                "nacks": network.get("nacks"),
                "lateArrivals": network.get("lateArrivals"),
                "terminalMissingSources": recovery.get(
                    "terminalMissingSources"),
                "recoveryEligibleSources": recovery.get(
                    "recoveryEligibleSources"),
                "recoverableGroups": recovery.get("recoverableGroups"),
                "recoveredSources": recovery.get("recoveredSources"),
                "recoveredGroups": recovery.get("recoveredGroups"),
                "recoveryAttempts": recovery.get("algorithmInvocations"),
                "recoveryExhaustions": recovery.get("recoveryExhaustions"),
                "nonproductiveInterestRatio": utility.get(
                    "nonproductiveInterestRatio", {}).get("value"),
                "protectionOnlyRatio": utility.get(
                    "protectionOnlyRatio", {}).get("value"),
                "interestConserved": utility.get("conserved"),
            })
    return campaign


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    args = parser.parse_args()
    result = analyze_root(args.input.resolve())
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
