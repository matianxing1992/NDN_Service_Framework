#!/usr/bin/env python3
"""Analyze one admissible Spec 111 paired MiniNDN campaign."""

from __future__ import annotations

import argparse
import json
import random
import statistics
from pathlib import Path
from typing import Iterable


ANALYSIS_SEED = 11195
BOOTSTRAP_REPETITIONS = 10_000
NON_REGRESSION_MARGIN = 0.05


def percentile(values: list[float], quantile: float) -> float:
    ordered = sorted(values)
    index = (len(ordered) - 1) * quantile
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = index - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def paired_relative(treatment: float, baseline: float) -> float:
    if baseline == 0:
        raise ValueError("SPEC111_ZERO_BASELINE_METRIC")
    return treatment / baseline - 1.0


def bootstrap_median_interval(
    values: list[float],
    *,
    seed: int = ANALYSIS_SEED,
    repetitions: int = BOOTSTRAP_REPETITIONS,
) -> list[float]:
    if not values:
        raise ValueError("SPEC111_EMPTY_BOOTSTRAP_INPUT")
    rng = random.Random(seed)
    estimates = [
        statistics.median(rng.choices(values, k=len(values)))
        for _ in range(repetitions)
    ]
    return [percentile(estimates, 0.025), percentile(estimates, 0.975)]


def median_or_none(values: Iterable[float | None]) -> float | None:
    available = [float(value) for value in values if value is not None]
    return statistics.median(available) if available else None


def analyze_metric(
    pairs: list[dict[str, dict[str, object]]],
    field: str,
) -> dict[str, object]:
    relative = [
        paired_relative(
            float(pair["treatment"][field]),
            float(pair["baseline"][field]),
        )
        for pair in pairs
    ]
    return {
        "baselineMedian": statistics.median(
            float(pair["baseline"][field]) for pair in pairs),
        "treatmentMedian": statistics.median(
            float(pair["treatment"][field]) for pair in pairs),
        "pairedRelativeChanges": relative,
        "medianPairedRelativeChange": statistics.median(relative),
        "bootstrap95": bootstrap_median_interval(relative),
    }


def analyze(summary: dict[str, object]) -> dict[str, object]:
    results = list(summary.get("results", []))
    # Campaigns started by the immediately preceding runner revision do not
    # carry this derived flag. They remain admissible only if every fact below
    # independently proves a clean single-candidate run. An explicit false is
    # permanent and always wins.
    if (
        summary.get("formalComparisonEligible") is False or
        summary.get("diagnosticContinuation")
    ):
        raise ValueError("SPEC111_CAMPAIGN_NOT_FORMALLY_ELIGIBLE")
    if len(results) != 20 or any(
            result.get("status") != "PASS" for result in results):
        raise ValueError("SPEC111_CAMPAIGN_NOT_20_OF_20_PASS")

    candidate_ids = {
        str(result["candidateId"])
        for result in results
        if result.get("variant") == "treatment"
    }
    if len(candidate_ids) != 1:
        raise ValueError("SPEC111_TREATMENT_CANDIDATE_NOT_SINGLE")

    indexed: dict[int, dict[str, dict[str, object]]] = {}
    for result in results:
        indexed.setdefault(int(result["pair"]), {})[
            str(result["variant"])
        ] = result
    pairs = [indexed[pair] for pair in range(1, 11)]
    if any(set(pair) != {"baseline", "treatment"} for pair in pairs):
        raise ValueError("SPEC111_INCOMPLETE_PAIR")

    metrics = {
        field: analyze_metric(pairs, field)
        for field in (
            "p50Ms", "p95Ms", "throughputRps", "peakProcessTreeRssKiB"
        )
    }
    queue = {
        variant: {
            "sampleCount": sum(
                int(pair[variant].get("queueSamples", 0)) for pair in pairs),
            "medianMaxObserved": median_or_none(
                pair[variant].get("maxQueueObserved") for pair in pairs),
        }
        for variant in ("baseline", "treatment")
    }
    latency_p50_pass = metrics["p50Ms"]["bootstrap95"][1] <= NON_REGRESSION_MARGIN
    latency_p95_pass = metrics["p95Ms"]["bootstrap95"][1] <= NON_REGRESSION_MARGIN
    throughput_pass = (
        metrics["throughputRps"]["bootstrap95"][0] >=
        -NON_REGRESSION_MARGIN
    )
    gate = {
        "correctnessCompletion": True,
        "latencyP50": latency_p50_pass,
        "latencyP95": latency_p95_pass,
        "throughput": throughput_pass,
    }
    gate["pass"] = all(gate.values())
    return {
        "schema": "ndnsf-di-spec111-paired-analysis-v1",
        "candidateId": next(iter(candidate_ids)),
        "analysisSeed": ANALYSIS_SEED,
        "bootstrapRepetitions": BOOTSTRAP_REPETITIONS,
        "nonRegressionMargin": NON_REGRESSION_MARGIN,
        "pairCount": 10,
        "baselineCompletedRequests": sum(
            int(pair["baseline"]["completedRequests"]) for pair in pairs),
        "treatmentCompletedRequests": sum(
            int(pair["treatment"]["completedRequests"]) for pair in pairs),
        "baselineFailedRequests": sum(
            int(pair["baseline"]["failedRequests"]) for pair in pairs),
        "treatmentFailedRequests": sum(
            int(pair["treatment"]["failedRequests"]) for pair in pairs),
        "metrics": metrics,
        "queue": queue,
        "gate": gate,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign-summary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    summary = json.loads(args.campaign_summary.read_text(encoding="utf-8"))
    result = analyze(summary)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(result, sort_keys=True))
    return 0 if result["gate"]["pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
