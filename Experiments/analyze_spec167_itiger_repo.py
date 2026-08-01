#!/usr/bin/env python3
"""Fail-closed analyzer for Spec 167 TigerCluster run ledgers."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
from typing import Any

from NDNSF_DistributedRepo_Artifact_Itiger import (
    PAYLOAD_SIZES, REPETITIONS, SUBJECTS, build_schedule,
)


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        value = json.loads(line)
        if not isinstance(value, dict):
            raise ValueError(f"{path}:{number}: record is not an object")
        rows.append(value)
    return rows


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, int((len(ordered) - 1) * fraction))
    return ordered[index]


def bootstrap_median(values: list[float], seed: int, draws: int = 10000) -> list[float]:
    if not values:
        return [0.0, 0.0]
    rng = random.Random(seed)
    medians = []
    for _ in range(draws):
        medians.append(statistics.median(rng.choice(values) for _ in values))
    medians.sort()
    return [medians[int(draws * 0.025)], medians[int(draws * 0.975) - 1]]


def analyze(manifest: dict[str, Any], rows: list[dict[str, Any]]) -> dict[str, Any]:
    expected = manifest["schedule"]
    expected_ids = [row["runId"] for row in expected]
    actual_ids = [str(row.get("runId", "")) for row in rows]
    duplicates = sorted({value for value in actual_ids if actual_ids.count(value) > 1})
    missing = sorted(set(expected_ids) - set(actual_ids))
    unexpected = sorted(set(actual_ids) - set(expected_ids))
    failures = [row for row in rows if row.get("status") != "PASS"]
    path_violations = []
    for row in rows:
        for key in ("dataPath", "storePath", "destinationPath"):
            value = row.get(key)
            if value and (str(value).startswith("/project/") or str(value).startswith("/home/")):
                path_violations.append({"runId": row.get("runId"), "field": key, "value": value})

    measured = [row for row in rows if not row.get("warmup", False)]
    cells: dict[str, Any] = {}
    for size in PAYLOAD_SIZES:
        by_subject: dict[str, list[float]] = {}
        for subject in SUBJECTS:
            values = [
                float(row.get("logicalGoodputMbps", 0.0))
                for row in measured
                if int(row.get("sizeBytes", -1)) == size
                and row.get("subject") == subject
            ]
            by_subject[subject] = values
        pairs = []
        for repetition in range(1, REPETITIONS + 1):
            item = {}
            for subject in SUBJECTS:
                match = [
                    row for row in measured
                    if int(row.get("sizeBytes", -1)) == size
                    and row.get("subject") == subject
                    and int(row.get("repetition", -1)) == repetition
                ]
                if len(match) == 1:
                    item[subject] = float(match[0].get("logicalGoodputMbps", 0.0))
            if len(item) == len(SUBJECTS):
                pairs.append(item)
        ratios = {
            "signedOverLegacy": [p["signed-manifest"] / p["legacy-exact-packet"] if p["legacy-exact-packet"] else 0.0 for p in pairs],
            "digestOverRaw": [p["digest-only"] / p["raw-segmented-ndn"] if p["raw-segmented-ndn"] else 0.0 for p in pairs],
            "signedOverDigest": [p["signed-manifest"] / p["digest-only"] if p["digest-only"] else 0.0 for p in pairs],
            "signedOverPhysical": [p["signed-manifest"] / p["physical-network"] if p["physical-network"] else 0.0 for p in pairs],
        }
        cells[str(size)] = {
            "subjects": {
                subject: {
                    "n": len(values),
                    "medianMbps": statistics.median(values) if values else 0.0,
                    "p50Mbps": percentile(values, 0.50),
                    "p95Mbps": percentile(values, 0.95),
                    "minimumMbps": min(values) if values else 0.0,
                    "maximumMbps": max(values) if values else 0.0,
                }
                for subject, values in by_subject.items()
            },
            "ratios": {
                name: {
                    "n": len(values),
                    "median": statistics.median(values) if values else 0.0,
                    "bootstrap95": bootstrap_median(values, 167 + size + index),
                }
                for index, (name, values) in enumerate(ratios.items())
            },
        }

    complete = not duplicates and not missing and not unexpected and not path_violations
    measured_failures = sum(1 for row in failures if not row.get("warmup", False))
    result = {
        "schemaVersion": 1,
        "campaignId": manifest.get("campaignId"),
        "expectedRuns": len(expected_ids),
        "observedRuns": len(rows),
        "expectedWarmups": len(SUBJECTS) * len(PAYLOAD_SIZES),
        "expectedMeasured": len(SUBJECTS) * len(PAYLOAD_SIZES) * REPETITIONS,
        "measuredFailures": measured_failures,
        "duplicates": duplicates,
        "missing": missing,
        "unexpected": unexpected,
        "pathViolations": path_violations,
        "cells": cells,
        "verdict": "PASS" if complete and measured_failures <= 2 else "FAIL",
    }
    result["analysisSha256"] = hashlib.sha256(
        json.dumps(result, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--runs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if [row["runId"] for row in manifest.get("schedule", [])] != [row["runId"] for row in build_schedule(int(manifest["randomizationSeed"]))]:
        raise SystemExit("manifest schedule does not match frozen generator")
    result = analyze(manifest, load_jsonl(args.runs))
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"SPEC167_ANALYSIS_{result['verdict']}")
    return 0 if result["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
