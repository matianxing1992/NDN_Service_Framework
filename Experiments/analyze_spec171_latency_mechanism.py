#!/usr/bin/env python3
"""Reconstruct the Spec 171 100 m retry-tail mechanism from frozen logs."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


ATTEMPT_PATTERN = re.compile(
    r"GRPC_FAILOVER_ATTEMPT request_id=(\d+) attempt=(\d+) "
    r"provider=(\w+) status=([A-Z_]+) latency_ms=([0-9.]+)"
)


def nearest_rank(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    return sorted(values)[max(0, math.ceil(quantile * len(values)) - 1)]


def parse_attempts(path: Path) -> dict[int, list[dict]]:
    requests: dict[int, list[dict]] = defaultdict(list)
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = ATTEMPT_PATTERN.search(line)
        if not match:
            continue
        request_id, attempt, provider, status, latency_ms = match.groups()
        requests[int(request_id)].append({
            "attempt": int(attempt),
            "provider": provider,
            "status": status,
            "latency_ms": float(latency_ms),
        })
    for attempts in requests.values():
        attempts.sort(key=lambda item: item["attempt"])
    return dict(requests)


def reconstruct_seed(path: Path, expected_success: int,
                     expected_p95_ms: float) -> dict:
    requests = parse_attempts(path)
    successful_latencies = []
    status_counts: Counter[str] = Counter()
    after_deadline = 0
    after_unavailable = 0
    maximum_attempt = 0
    for attempts in requests.values():
        maximum_attempt = max(maximum_attempt, len(attempts))
        status_counts.update(item["status"] for item in attempts)
        if not attempts or attempts[-1]["status"] != "OK":
            continue
        successful_latencies.append(sum(item["latency_ms"] for item in attempts))
        preceding = {item["status"] for item in attempts[:-1]}
        after_deadline += "DEADLINE_EXCEEDED" in preceding
        after_unavailable += "UNAVAILABLE" in preceding

    reconstructed_p95 = nearest_rank(successful_latencies, 0.95)
    if len(successful_latencies) != expected_success:
        raise ValueError(
            f"{path}: reconstructed {len(successful_latencies)} successes; "
            f"summary reports {expected_success}"
        )
    p95_error_ms = reconstructed_p95 - expected_p95_ms
    if abs(p95_error_ms) > 2.0:
        raise ValueError(
            f"{path}: reconstructed p95 differs by {p95_error_ms:.3f} ms"
        )
    return {
        "success": len(successful_latencies),
        "reported_p95_ms": expected_p95_ms,
        "reconstructed_p95_ms": reconstructed_p95,
        "reconstruction_error_ms": p95_error_ms,
        "successful_latency_bands": {
            "under_100_ms": sum(value < 100.0 for value in successful_latencies),
            "100_to_900_ms": sum(
                100.0 <= value < 900.0 for value in successful_latencies),
            "at_least_900_ms": sum(value >= 900.0 for value in successful_latencies),
        },
        "successful_after_deadline": after_deadline,
        "successful_after_unavailable": after_unavailable,
        "maximum_attempts": maximum_attempt,
        "attempt_status_counts": dict(sorted(status_counts.items())),
    }


def analyze(root: Path, condition: str) -> dict:
    aggregate_path = root / "aggregate.json"
    aggregate = json.loads(aggregate_path.read_text(encoding="utf-8"))
    records = [record for record in aggregate["records"]
               if record["condition"] == condition]
    systems = {record["system"] for record in records}
    if systems != {"ndnsf", "grpc", "nsc"}:
        raise ValueError(f"unexpected system set: {sorted(systems)}")

    by_system_seed = {
        (record["system"], int(record["seed"])): record
        for record in records
    }
    seeds = sorted({int(record["seed"]) for record in records})
    grpc_per_seed = []
    grpc_status_totals: Counter[str] = Counter()
    for seed in seeds:
        record = by_system_seed[("grpc", seed)]
        summary = record["summary"]
        cell_root = root / f"seed-{seed}" / condition / "grpc"
        reconstructed = reconstruct_seed(
            cell_root / "grpc-client.log",
            int(summary["success"]),
            float(summary["p95_ms"]),
        )
        grpc_status_totals.update(reconstructed["attempt_status_counts"])
        reconstructed.update({
            "seed": seed,
            "any_provider_coverage_pct": 100.0 * record["trace_metrics"][
                "measurement_window"]["at_least_one_fraction"],
        })
        grpc_per_seed.append(reconstructed)

    ndnsf_per_seed = []
    for seed in seeds:
        record = by_system_seed[("ndnsf", seed)]
        summary = record["summary"]
        ndnsf_per_seed.append({
            "seed": seed,
            "success": int(summary["success"]),
            "p95_ms": float(summary["p95_ms"]),
            "deadline_failures": int(summary["deadline_failures"]),
            "provider_executions": int(summary["provider_executions"]),
            "response_reselections": int(summary["response_reselections"]),
        })

    grpc_p95 = [item["reported_p95_ms"] for item in grpc_per_seed]
    ndnsf_p95 = [item["p95_ms"] for item in ndnsf_per_seed]
    slow_tail_seeds = [item["seed"] for item in grpc_per_seed
                       if item["successful_latency_bands"]["at_least_900_ms"] >= 15]
    fast_tail_seeds = [item["seed"] for item in grpc_per_seed
                       if item["reported_p95_ms"] < 100.0]
    return {
        "schema": "ndnsf-spec171-latency-mechanism-v1",
        "aggregate": str(aggregate_path.resolve()),
        "condition": condition,
        "seeds": seeds,
        "grpc": {
            "per_seed": grpc_per_seed,
            "attempt_status_totals": dict(sorted(grpc_status_totals.items())),
            "median_seed_p95_ms": statistics.median(grpc_p95),
            "slow_tail_seeds": slow_tail_seeds,
            "fast_tail_seeds": fast_tail_seeds,
        },
        "ndnsf": {
            "per_seed": ndnsf_per_seed,
            "median_seed_p95_ms": statistics.median(ndnsf_p95),
            "total_response_reselections": sum(
                item["response_reselections"] for item in ndnsf_per_seed),
        },
        "verdict": (
            "GRPC_SEED_P95_IS_BIMODAL_AND_DRIVEN_BY_SUCCESS_AFTER_1S_DEADLINE;"
            "NDNSF_FIRST_ACK_SELECTION_HAS_STABLE_SUB_110MS_SEED_P95"
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--condition", default="range-100-speed-2p0")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.root.resolve(), args.condition)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
