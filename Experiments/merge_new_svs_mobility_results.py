#!/usr/bin/env python3
"""Merge the bounded new-SVS one-AP mobility cells into one auditable report."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


SYSTEMS = ("ndnsf", "grpc", "nsc")
CONDITIONS = (
    (50.0, 2.0),
    (100.0, 2.0),
    (50.0, 15.0),
    (100.0, 15.0),
)
SEEDS = (40, 41, 42)


def condition_id(range_m: float, speed_mps: float) -> str:
    return f"range-{int(range_m)}-speed-{str(speed_mps).replace('.', 'p')}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_root(roots: dict[str, Path], seed: int, range_m: float, speed_mps: float) -> Path:
    if speed_mps == 15.0:
        return roots["speed15"]
    if seed == 40:
        return roots["speed2_seed40_range50" if range_m == 50.0
                     else "speed2_seed40_range100"]
    return roots["speed2_seed41_42"]


def read_cell(roots: dict[str, Path], seed: int, range_m: float,
              speed_mps: float, system: str) -> dict[str, Any]:
    root = source_root(roots, seed, range_m, speed_mps)
    cid = condition_id(range_m, speed_mps)
    cell = root / f"seed-{seed}" / cid / system
    summary_path = cell / "summary.json"
    manifest_path = cell / "cell-manifest.json"
    trace_info_path = cell.parent / "trace-info.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))["summaries"][0]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    trace_info = json.loads(trace_info_path.read_text(encoding="utf-8"))
    if summary.get("status") != "passed":
        raise ValueError(f"cell did not pass: {cell}")
    if summary.get("sent") != 300:
        raise ValueError(f"cell contract mismatch: {cell}")
    if system == "ndnsf" and summary.get("admission_control") != "disabled":
        raise ValueError(f"NDNSF admission contract mismatch: {cell}")
    if manifest.get("trace_sha256") != trace_info.get("sha256"):
        raise ValueError(f"trace hash mismatch: {cell}")
    return {
        "seed": seed,
        "condition": cid,
        "range_m": range_m,
        "speed_mps": speed_mps,
        "system": system,
        "summary": summary,
        "trace_sha256": trace_info["sha256"],
        "trace_metrics": trace_info["measurement_window"],
        "summary_path": str(summary_path.resolve()),
        "manifest_path": str(manifest_path.resolve()),
        "trace_info_path": str(trace_info_path.resolve()),
    }


def merge(roots: dict[str, Path]) -> dict[str, Any]:
    records = []
    for range_m, speed_mps in CONDITIONS:
        for seed in SEEDS:
            for system in SYSTEMS:
                records.append(read_cell(roots, seed, range_m, speed_mps, system))

    by_condition: dict[str, Any] = {}
    for range_m, speed_mps in CONDITIONS:
        cid = condition_id(range_m, speed_mps)
        selected = [r for r in records if r["condition"] == cid]
        systems: dict[str, Any] = {}
        for system in SYSTEMS:
            rows = [r for r in selected if r["system"] == system]
            requests = sum(r["summary"]["sent"] for r in rows)
            success = sum(r["summary"]["success"] for r in rows)
            successful_latency = sum(
                r["summary"]["mean_ms"] * r["summary"]["success"] for r in rows)
            systems[system] = {
                "requests": requests,
                "success": success,
                "success_rate": success / requests,
                "mean_success_latency_ms": successful_latency / success,
                "mean_p50_ms": sum(r["summary"]["p50_ms"] for r in rows) / len(rows),
                "mean_p95_ms": sum(r["summary"]["p95_ms"] for r in rows) / len(rows),
                "per_seed": [
                    {
                        "seed": r["seed"],
                        "success": r["summary"]["success"],
                        "requests": r["summary"]["sent"],
                        "success_rate": r["summary"]["success"] / r["summary"]["sent"],
                        "mean_ms": r["summary"]["mean_ms"],
                        "p95_ms": r["summary"]["p95_ms"],
                    }
                    for r in rows
                ],
            }
        coverage = [
            r["trace_metrics"] for r in selected if r["system"] == "ndnsf"
        ]
        diffs = {}
        for baseline in ("grpc", "nsc"):
            diffs[baseline] = [
                row["ndnsf"]["success_rate"] - row[baseline]["success_rate"]
                for row in (
                    {s: next(x for x in systems[s]["per_seed"] if x["seed"] == seed)
                     for s in SYSTEMS}
                    for seed in SEEDS
                )
            ]
        by_condition[cid] = {
            "range_m": range_m,
            "speed_mps": speed_mps,
            "systems": systems,
            "coverage_mean": {
                key: sum(item[key] for item in coverage) / len(coverage)
                for key in ("at_least_one_fraction", "all_unreachable_fraction",
                            "at_least_two_fraction")
            },
            "paired_success_difference": diffs,
            "trace_hashes": sorted({r["trace_sha256"] for r in selected}),
        }
    return {
        "schema": "ndnsf-new-svs-mobility-aggregate-v1",
        "systems": list(SYSTEMS),
        "seeds": list(SEEDS),
        "conditions": [condition_id(*item) for item in CONDITIONS],
        "contract": {
            "profile": "four-provider-single-ap",
            "ap_layout": "single",
            "duration_s": 60,
            "rate_rps": 5.0,
            "attempt_timeout_ms": 1000,
            "ack_timeout_ms": 1000,
            "global_deadline_ms": 5000,
            "block_network": True,
            "admission_control": "disabled",
            "grpc_health_oracle": "disabled",
            "ndnsf_strategy": "first-responding",
        },
        "source_roots": {key: str(value.resolve()) for key, value in roots.items()},
        "conditions": by_condition,
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--speed2-seed40-range50", type=Path, required=True)
    parser.add_argument("--speed2-seed40-range100", type=Path, required=True)
    parser.add_argument("--speed2-seed41-42", type=Path, required=True)
    parser.add_argument("--speed15", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    roots = {
        "speed2_seed40_range50": args.speed2_seed40_range50,
        "speed2_seed40_range100": args.speed2_seed40_range100,
        "speed2_seed41_42": args.speed2_seed41_42,
        "speed15": args.speed15,
    }
    report = merge(roots)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps({"output": str(args.output.resolve()),
                      "records": len(report["records"]),
                      "conditions": report["conditions"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
