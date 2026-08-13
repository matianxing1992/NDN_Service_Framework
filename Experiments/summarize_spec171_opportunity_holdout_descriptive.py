#!/usr/bin/env python3
"""Produce secondary descriptive metrics for the frozen Spec 171 holdout.

This script does not change the preregistered p95-latency confirmation gate.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import random
import statistics


SYSTEMS = ("ndnsf", "grpc", "nsc")
BASELINES = ("grpc", "nsc")
EXPECTED_SEEDS = list(range(72, 82))


def paired_bootstrap_mean(
        values: list[float], *, seed: int = 171,
        repetitions: int = 20000) -> dict:
    if not values:
        raise ValueError("paired bootstrap requires at least one seed")
    rng = random.Random(seed)
    draws = sorted(statistics.mean(rng.choices(values, k=len(values)))
                   for _ in range(repetitions))
    return {
        "mean": statistics.mean(values),
        "ci95_low": draws[math.floor(0.025 * repetitions)],
        "ci95_high": draws[math.ceil(0.975 * repetitions) - 1],
        "seed": seed,
        "repetitions": repetitions,
        "per_seed": values,
    }


def summarize(primary: dict) -> dict:
    per_seed = primary["per_seed"]
    seeds = [item["seed"] for item in per_seed]
    if seeds != EXPECTED_SEEDS:
        raise ValueError(f"expected holdout seeds {EXPECTED_SEEDS}, got {seeds}")

    aggregate = {}
    for system in SYSTEMS:
        requests = sum(item[system]["requests"] for item in per_seed)
        successes = sum(item[system]["success"] for item in per_seed)
        attempts = sum(item[system]["attempts_or_executions"] for item in per_seed)
        aggregate[system] = {
            "requests": requests,
            "successes": successes,
            "success_rate_pct": 100.0 * successes / requests,
            "attempts_or_executions": attempts,
            "attempts_or_executions_per_request": attempts / requests,
        }

    success_differences = {}
    for baseline in BASELINES:
        differences = [
            100.0 * (
                item["ndnsf"]["success"] / item["ndnsf"]["requests"] -
                item[baseline]["success"] / item[baseline]["requests"])
            for item in per_seed
        ]
        success_differences[baseline] = paired_bootstrap_mean(differences)

    return {
        "schema": "spec171-opportunity-holdout-descriptive-v1",
        "source_schema": primary["schema"],
        "source_verdict": primary["verdict"],
        "scope": "agreed SWITCH_REQUIRED rows",
        "status": "post-hoc descriptive; not part of the registered confirmation gate",
        "aggregate": aggregate,
        "paired_ndnsf_minus_baseline_success_rate_percentage_points":
            success_differences,
        "count_semantics": (
            "NDNSF counts selected Provider executions; gRPC and NSC count "
            "sequential endpoint attempts. These expose mechanism cost but are "
            "not identical wire-message units."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("primary_summary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    primary = json.loads(args.primary_summary.read_text())
    report = summarize(primary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({
        "source_verdict": report["source_verdict"],
        "switch_required_requests": report["aggregate"]["ndnsf"]["requests"],
    }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
