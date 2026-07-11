#!/usr/bin/env python3
"""Run a small runtime-aware NDNSF-DI MiniNDN RPS sweep.

The sweep intentionally wraps the canonical NativeTracer MiniNDN launcher
instead of reimplementing topology, controller, provider, or user logic.
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
DEFAULT_WORKLOAD = (
    REPO /
    "examples/python/NDNSF-DistributedInference/native_di_tracer/runtime_aware_fixtures/multi_user_requests.json"
)


def parse_rps_values(text: str) -> list[float]:
    values: list[float] = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        value = float(item)
        if value <= 0.0:
            raise argparse.ArgumentTypeError("RPS values must be positive")
        values.append(value)
    if not values:
        raise argparse.ArgumentTypeError("at least one RPS value is required")
    return values


def load_json(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def point_summary(point_dir: Path, target_rps: float, returncode: int, mode: str) -> dict[str, Any]:
    summary = load_json(point_dir / "summary.json")
    metrics = load_json(point_dir / "planner-metrics.json")
    user = summary.get("userExecution", {}) if isinstance(summary.get("userExecution"), dict) else {}
    request_count = int(user.get("requestCount", summary.get("requestCount", 0)) or 0)
    success_count = int(user.get("successCount", 0) or 0)
    success_rate = round(success_count / request_count, 6) if request_count else 0.0
    failure_rate = round(1.0 - success_rate, 6) if request_count else 1.0
    return {
        "targetRps": target_rps,
        "mode": mode,
        "returncode": returncode,
        "status": summary.get("status", "FAILURE" if returncode else "UNKNOWN"),
        "stable": returncode == 0 and success_rate >= 0.99,
        "successRate": success_rate,
        "failureRate": failure_rate,
        "requestCount": request_count,
        "successCount": success_count,
        "p50Ms": float(user.get("p50Ms", 0.0) or 0.0),
        "p95Ms": float(user.get("p95Ms", 0.0) or 0.0),
        "throughputRps": float(user.get("throughputRps", 0.0) or 0.0),
        "overloadFastFailCount": int(user.get("overloadFastFailCount", 0) or 0),
        "overloadFastFail": user.get("overloadFastFail", {}),
        "leaseCounters": metrics.get("leaseCounters", summary.get("leaseCounters", {})),
        "residencyCounters": metrics.get("residencyCounters", {}),
        "observedResidencyCounters": metrics.get("observedResidencyCounters", {}),
        "providerFragmentInventory": metrics.get(
            "providerFragmentInventory",
            summary.get("providerFragmentInventory", {}),
        ),
        "summaryPath": str(point_dir / "summary.json"),
        "metricsPath": str(point_dir / "planner-metrics.json"),
    }


def write_outputs(out_dir: Path, points: list[dict[str, Any]]) -> None:
    stable_points = [item for item in points if item.get("stable")]
    max_stable_by_mode = {
        mode: max(
            (float(item["targetRps"]) for item in stable_points if item.get("mode") == mode),
            default=0.0,
        )
        for mode in sorted({str(item.get("mode", "default")) for item in points})
    }
    max_stable = max(max_stable_by_mode.values(), default=0.0)
    payload = {
        "status": "SUCCESS" if points else "FAILURE",
        "pointCount": len(points),
        "maxStableRps": max_stable,
        "maxStableRpsByMode": max_stable_by_mode,
        "points": points,
    }
    (out_dir / "rps-sweep-summary.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    with (out_dir / "rps-sweep-summary.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=[
                "mode", "targetRps", "status", "stable", "successRate", "failureRate",
                "requestCount", "successCount", "p50Ms", "p95Ms", "throughputRps",
                "overloadFastFailCount",
                "summaryPath", "metricsPath",
            ],
        )
        writer.writeheader()
        for item in points:
            writer.writerow({
                key: item.get(key, "")
                for key in writer.fieldnames
            })


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, help="Output directory for sweep results")
    parser.add_argument("--rps", type=parse_rps_values, required=True,
                        help="Comma-separated target RPS values, e.g. 0.2,0.4")
    parser.add_argument("--requests", type=int, default=2)
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--open-loop-duration-s", type=float, default=0.0)
    parser.add_argument("--workload", default=str(DEFAULT_WORKLOAD))
    parser.add_argument("--disable-native-admission-lease", action="store_true",
                        help="Run the sweep without NativeTracer generic admission leases")
    parser.add_argument("--capacity-pool", action="store_true",
                        help="Use NativeTracer capacity-pool assignment with multiple candidates per role")
    parser.add_argument("--overload-fast-fail-timeout-ms", type=int, default=0,
                        help="Forward a shorter collaboration timeout for overload fast-fail evidence")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("extra_harness_args", nargs=argparse.REMAINDER,
                        help="Arguments appended to each NativeTracer MiniNDN run")
    args = parser.parse_args(argv)
    if args.overload_fast_fail_timeout_ms < 0:
        raise SystemExit("--overload-fast-fail-timeout-ms must be non-negative")

    out_dir = Path(args.out).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    points: list[dict[str, Any]] = []
    commands: list[list[str]] = []
    extras = list(args.extra_harness_args)
    if extras and extras[0] == "--":
        extras = extras[1:]

    for mode in ["pure"]:
        for target_rps in args.rps:
            point_dir = out_dir / mode / f"rps-{str(target_rps).replace('.', 'p')}"
            cmd = [
                sys.executable,
                str(HARNESS),
                "--out", str(point_dir),
                "--runtime-aware-user-planner",
                "--multi-user-workload", args.workload,
                "--runtime-aware-max-replans", "1",
                "--runtime-aware-replan-reasons", "FRAGMENT_EVICTED",
                "--requests", str(args.requests),
                "--concurrency", str(args.concurrency),
                "--target-rps", str(target_rps),
                "--full-network",
                "--tracer-deterministic-runner",
            ]
            if not args.disable_native_admission_lease:
                cmd.append("--enable-native-admission-lease")
            if args.capacity_pool:
                cmd.extend(["--assignment", "capacity-pool"])
            if args.overload_fast_fail_timeout_ms > 0:
                cmd.extend([
                    "--overload-fast-fail-timeout-ms",
                    str(args.overload_fast_fail_timeout_ms),
                ])
            if args.open_loop_duration_s > 0.0:
                cmd.extend(["--open-loop-duration-s", str(args.open_loop_duration_s)])
            cmd.extend(extras)
            commands.append(cmd)
            if args.dry_run:
                continue
            completed = subprocess.run(cmd, cwd=str(REPO))
            points.append(point_summary(point_dir, target_rps, completed.returncode, mode))

    (out_dir / "rps-sweep-commands.json").write_text(
        json.dumps(commands, indent=2) + "\n",
        encoding="utf-8")
    if args.dry_run:
        print(json.dumps({"status": "DRY_RUN", "commands": commands}, indent=2))
        return 0
    write_outputs(out_dir, points)
    print((out_dir / "rps-sweep-summary.json").read_text(encoding="utf-8"))
    return 0 if points and all(int(item["returncode"]) == 0 for item in points) else 1


if __name__ == "__main__":
    raise SystemExit(main())
