#!/usr/bin/env python3
"""Merge paired new-SVS range/speed timeout matrix roots.

The two input roots must contain the same (range, speed, seed) traces and
three systems.  Timeout is the only intended difference between roots.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any


SYSTEMS = ("ndnsf", "grpc", "nsc")
CONDITION_RE = re.compile(r"^range-(?P<range>[0-9]+)-speed-(?P<speed>[0-9]+p[0-9]+)$")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_condition(value: str) -> tuple[float, float]:
    match = CONDITION_RE.fullmatch(value)
    if match is None:
        raise ValueError(f"invalid condition directory: {value}")
    speed = float(match.group("speed").replace("p", "."))
    return float(match.group("range")), speed


def first_summary(path: Path) -> dict[str, Any]:
    envelope = json.loads(path.read_text(encoding="utf-8"))
    summaries = envelope.get("summaries", [])
    if len(summaries) != 1:
        raise ValueError(f"summary must contain exactly one result: {path}")
    return summaries[0]


def read_cell(root: Path, timeout_ms: int, seed: int, condition: str,
              system: str) -> dict[str, Any]:
    cell = root / f"seed-{seed}" / condition / system
    summary_path = cell / "summary.json"
    manifest_path = cell / "cell-manifest.json"
    trace_info_path = cell.parent / "trace-info.json"
    if not summary_path.is_file() or not manifest_path.is_file() or not trace_info_path.is_file():
        raise ValueError(f"missing cell evidence: {cell}")
    summary = first_summary(summary_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    trace_info = json.loads(trace_info_path.read_text(encoding="utf-8"))
    if summary.get("status") != "passed" or summary.get("sent") != 300:
        raise ValueError(f"cell did not pass the 300-request contract: {cell}")
    for key, expected in (("global_deadline_ms", 5000),
                          ("attempt_timeout_ms", timeout_ms),
                          ("ack_timeout_ms", timeout_ms)):
        if manifest.get(key) != expected:
            raise ValueError(f"{key} mismatch in {manifest_path}: {manifest.get(key)}")
    if manifest.get("admission_control") != "disabled":
        raise ValueError(f"admission policy mismatch in {manifest_path}")
    if manifest.get("trace_sha256") != trace_info.get("sha256"):
        raise ValueError(f"trace hash mismatch in {manifest_path}")
    if system == "ndnsf":
        runtime = manifest.get("ndnsf_runtime")
        if not runtime or not runtime.get("svs_library_sha256"):
            raise ValueError(f"missing NDNSF runtime provenance in {manifest_path}")
    return {
        "root": str(root.resolve()),
        "timeout_ms": timeout_ms,
        "seed": seed,
        "condition": condition,
        "range_m": manifest["range_m"],
        "speed_mps": manifest["speed_mps"],
        "system": system,
        "summary": summary,
        "manifest": manifest,
        "trace_sha256": trace_info["sha256"],
        "trace_metrics": trace_info["measurement_window"],
        "summary_path": str(summary_path.resolve()),
        "manifest_path": str(manifest_path.resolve()),
        "trace_info_path": str(trace_info_path.resolve()),
    }


def discover(root: Path) -> tuple[list[int], list[str]]:
    seeds = sorted(int(path.name[len("seed-"):])
                   for path in root.glob("seed-*") if path.is_dir())
    conditions = sorted({path.name for seed in root.glob("seed-*") if seed.is_dir()
                         for path in seed.iterdir() if path.is_dir()})
    if not seeds or not conditions:
        raise ValueError(f"root has no seed/condition directories: {root}")
    return seeds, conditions


def collect(root: Path, timeout_ms: int) -> list[dict[str, Any]]:
    seeds, conditions = discover(root)
    records = []
    for seed in seeds:
        for condition in conditions:
            parse_condition(condition)
            for system in SYSTEMS:
                records.append(read_cell(root, timeout_ms, seed, condition, system))
    return records


def system_aggregate(rows: list[dict[str, Any]]) -> dict[str, Any]:
    requests = sum(int(row["summary"]["sent"]) for row in rows)
    success = sum(int(row["summary"]["success"]) for row in rows)
    successful_latency = sum(float(row["summary"]["mean_ms"]) *
                             int(row["summary"]["success"]) for row in rows)
    return {
        "requests": requests,
        "success": success,
        "success_rate": success / requests if requests else None,
        "mean_success_latency_ms": successful_latency / success if success else None,
        "mean_p50_ms": sum(float(row["summary"]["p50_ms"]) for row in rows) / len(rows),
        "mean_p95_ms": sum(float(row["summary"]["p95_ms"]) for row in rows) / len(rows),
        "per_seed": [
            {"seed": row["seed"], "success": row["summary"]["success"],
             "requests": row["summary"]["sent"],
             "success_rate": row["summary"]["success"] / row["summary"]["sent"],
             "mean_ms": row["summary"]["mean_ms"],
             "p95_ms": row["summary"]["p95_ms"]}
            for row in sorted(rows, key=lambda item: item["seed"])
        ],
    }


def aggregate(records: list[dict[str, Any]]) -> dict[str, Any]:
    roots = sorted({row["root"] for row in records})
    timeouts = sorted({row["timeout_ms"] for row in records})
    conditions = sorted({row["condition"] for row in records})
    by_condition: dict[str, Any] = {}
    for condition in conditions:
        selected = [row for row in records if row["condition"] == condition]
        by_timeout: dict[str, Any] = {}
        for timeout in timeouts:
            timed = [row for row in selected if row["timeout_ms"] == timeout]
            systems = {
                system: system_aggregate([row for row in timed if row["system"] == system])
                for system in SYSTEMS
            }
            paired = {}
            for baseline in ("grpc", "nsc"):
                nd = {row["seed"]: row["summary"]["success"] / row["summary"]["sent"]
                      for row in timed if row["system"] == "ndnsf"}
                base = {row["seed"]: row["summary"]["success"] / row["summary"]["sent"]
                        for row in timed if row["system"] == baseline}
                paired[baseline] = [nd[seed] - base[seed]
                                    for seed in sorted(set(nd) & set(base))]
            coverage = [row["trace_metrics"] for row in timed if row["system"] == "ndnsf"]
            by_timeout[str(timeout)] = {
                "systems": systems,
                "paired_success_difference": paired,
                "coverage_mean": {
                    key: sum(item[key] for item in coverage) / len(coverage)
                    for key in ("at_least_one_fraction", "all_unreachable_fraction",
                                "at_least_two_fraction")
                },
                "trace_hashes": sorted({row["trace_sha256"] for row in timed}),
            }
        deltas = {}
        if set(timeouts) == {500, 1000}:
            for system in SYSTEMS:
                low = by_timeout["500"]["systems"][system]
                high = by_timeout["1000"]["systems"][system]
                deltas[system] = {
                    "success_rate_1000_minus_500": high["success_rate"] - low["success_rate"],
                    "mean_latency_1000_minus_500_ms": (
                        high["mean_success_latency_ms"] - low["mean_success_latency_ms"]),
                }
        by_condition[condition] = {
            "range_m": parse_condition(condition)[0],
            "speed_mps": parse_condition(condition)[1],
            "by_timeout_ms": by_timeout,
            "timeout_delta_1000_minus_500": deltas,
        }
    return {
        "schema": "ndnsf-new-svs-range-speed-timeout-aggregate-v1",
        "systems": list(SYSTEMS), "timeouts_ms": timeouts,
        "conditions": conditions, "source_roots": roots,
        "contract": {"global_deadline_ms": 5000, "duration_s": 60,
                     "rate_rps": 5, "block_network": True,
                     "admission_control": "disabled",
                     "grpc_health_oracle": "disabled",
                     "ndnsf_strategy": "first-responding"},
        "conditions_report": by_condition,
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout-500-root", type=Path, required=True)
    parser.add_argument("--timeout-1000-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    low = collect(args.timeout_500_root, 500)
    high = collect(args.timeout_1000_root, 1000)
    low_keys = {(row["seed"], row["condition"], row["system"]): row for row in low}
    high_keys = {(row["seed"], row["condition"], row["system"]): row for row in high}
    if set(low_keys) != set(high_keys):
        raise ValueError("timeout roots do not contain the same seed/condition/system cells")
    for key in low_keys:
        if low_keys[key]["trace_sha256"] != high_keys[key]["trace_sha256"]:
            raise ValueError(f"trace mismatch between timeout roots: {key}")
    report = aggregate(low + high)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps({"output": str(args.output.resolve()),
                      "records": len(report["records"]),
                      "conditions": report["conditions"],
                      "timeouts_ms": report["timeouts_ms"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
