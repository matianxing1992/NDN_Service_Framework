#!/usr/bin/env python3
"""Run and aggregate paired four-Provider mobility repetitions.

Each seed is one immutable three-system campaign.  The harness creates one
trace for the campaign and replays that byte-identical trace to NDNSF, gRPC,
and NSC.  This driver never reruns a terminal or failed campaign directory;
failed/missing triplets remain visible in the aggregate report.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import sys
import time


SYSTEMS = ("ndnsf", "grpc", "nsc")
CONDITIONS = {
    "moderate": {
        "range_m": 100.0,
        "speed_mps": 8.0,
        "timeout_ms": 5000,
        "attempt_timeout_ms": 200,
        "health_interval_ms": 200,
    },
    "stale-health": {
        "range_m": 75.0,
        "speed_mps": 15.0,
        "timeout_ms": 300,
        "attempt_timeout_ms": 100,
        "health_interval_ms": 1000,
    },
}


def enforce_disk_budget(path: Path, min_free_gb: float, phase: str) -> int:
    """Refuse a campaign when its filesystem margin is below the budget.

    Mobility cells are small, but this runner may launch many sequential
    MiniNDN cells and write logs/manifests for each one.  A single cheap
    preflight prevents an unrelated artifact cache from turning a long
    campaign into an ENOSPC incident without adding runtime polling.
    """
    if min_free_gb < 0 or not math.isfinite(min_free_gb):
        raise ValueError("min-free-gb must be finite and non-negative")
    free_bytes = shutil.disk_usage(path).free
    required_bytes = int(min_free_gb * (1024 ** 3))
    if free_bytes < required_bytes:
        raise RuntimeError(
            f"disk budget check failed {phase}: {path} has "
            f"{free_bytes / (1024 ** 3):.2f} GiB free; "
            f"requires at least {min_free_gb:.2f} GiB")
    return free_bytes


def parse_ints(value: str) -> list[int]:
    result = [int(item.strip()) for item in value.split(",") if item.strip()]
    if not result:
        raise argparse.ArgumentTypeError("seed list must not be empty")
    if len(set(result)) != len(result):
        raise argparse.ArgumentTypeError("seed list contains duplicates")
    return result


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def percentile(values: list[float], probability: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(probability * len(ordered)) - 1))
    return ordered[index]


def wilson(success: int, total: int, z: float = 1.959963984540054) -> dict:
    if total <= 0:
        return {"estimate": None, "lower": None, "upper": None}
    p = success / total
    denominator = 1.0 + z * z / total
    center = (p + z * z / (2.0 * total)) / denominator
    half = z * math.sqrt(p * (1.0 - p) / total + z * z / (4.0 * total * total)) / denominator
    return {"estimate": p, "lower": max(0.0, center - half), "upper": min(1.0, center + half)}


def paired_bootstrap(rows: list[dict], baseline: str, iterations: int, seed: int) -> dict:
    differences = [
        row["systems"]["ndnsf"]["success_rate"] - row["systems"][baseline]["success_rate"]
        for row in rows
    ]
    if not differences:
        return {"n": 0, "mean": None, "median": None, "lower": None, "upper": None}
    rng = random.Random(seed)
    samples = []
    count = len(differences)
    for _ in range(max(1000, iterations)):
        samples.append(statistics.mean(differences[rng.randrange(count)] for _ in range(count)))
    return {
        "n": count,
        "mean": statistics.mean(differences),
        "median": statistics.median(differences),
        "lower": percentile(samples, 0.025),
        "upper": percentile(samples, 0.975),
    }


def metric_from_result(result: dict) -> dict:
    sent = int(result["sent"])
    success = int(result["success"])
    attempts = int(result.get("attempts", 0))
    if sent <= 0 or not 0 <= success <= sent or attempts < 0:
        raise ValueError(f"invalid result accounting: {result}")
    return {
        "system_label": result.get("system_label"),
        "sent": sent,
        "success": success,
        "success_rate": success / sent,
        "deadline_failures": int(result.get("deadline_failures", result.get("timeout", 0))),
        "attempts": attempts,
        "attempts_per_request": attempts / sent,
        "failovers": int(result.get("failovers", 0)),
        "failovers_per_request": int(result.get("failovers", 0)) / sent,
        "p50_ms": result.get("p50_ms"),
        "p95_ms": result.get("p95_ms"),
        "p99_ms": result.get("p99_ms"),
        "provider_executions": result.get("provider_execution_counts", result.get("provider_attempts")),
        "ndnsf_strategy": result.get("ndnsf_strategy"),
        "health_routing": result.get("health_routing"),
        "execution_mode": result.get("execution_mode"),
        "parallel_issued": int(result.get("parallel_issued", 0)),
        "parallel_winners": int(result.get("parallel_winners", 0)),
        "parallel_cancellations": int(result.get("parallel_cancellations", 0)),
        "server_extra_executions_per_request": int(
            result.get("server_extra_executions_per_request_exact", 0)),
    }


def read_campaign(campaign_dir: Path, condition: str, seed: int) -> dict:
    summary_path = campaign_dir / "campaign-summary.json"
    manifest_path = campaign_dir / "campaign-manifest.json"
    record = {
        "condition": condition,
        "seed": seed,
        "campaign_dir": str(campaign_dir.resolve()),
        "summary_file": str(summary_path.resolve()),
        "manifest_file": str(manifest_path.resolve()),
        "status": "missing",
        "trace_hashes": [],
    }
    if not summary_path.is_file() or not manifest_path.is_file():
        return record
    try:
        summary = json.loads(summary_path.read_text())
        manifest = json.loads(manifest_path.read_text())
        cells = summary.get("cells", [])
        by_system = {cell.get("system_id"): cell for cell in cells}
        traces = {cell.get("trace_sha256") for cell in cells}
        record.update({
            "status": summary.get("status", "unknown"),
            "trace_hashes": sorted(item for item in traces if item),
            "campaign_id": manifest.get("campaign_id"),
            "manifest_sha256": sha256_file(manifest_path),
            "configuration": manifest.get("configuration", {}),
        })
        if set(by_system) != set(SYSTEMS) or len(traces) != 1:
            record["status"] = "incomplete"
            record["error"] = "missing system or non-paired trace hashes"
            return record
        if any(cell.get("status") != "passed" for cell in by_system.values()):
            record["status"] = "failed"
            record["error"] = "at least one system cell did not pass"
            return record
        record["systems"] = {
            system: metric_from_result(by_system[system]) for system in SYSTEMS
        }
        record["status"] = "complete"
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        record["status"] = "invalid"
        record["error"] = str(error)
    return record


def command_for(args, condition: str, seed: int, campaign_dir: Path) -> list[str]:
    settings = CONDITIONS[condition]
    harness = Path(args.harness).resolve()
    lock_path = Path(args.lock_file).resolve()
    command = [
        sys.executable, str(harness),
        "--profile", "four-provider-multi-ap",
        "--ap-layout", "multi-ap",
        "--systems", ",".join(SYSTEMS),
        "--include-ndnsf",
        "--ranges", str(settings["range_m"]),
        "--speed-mps", str(settings["speed_mps"]),
        "--duration-s", str(args.duration_s),
        "--rate-rps", str(args.rate_rps),
        "--processing-delay-ms", str(args.processing_delay_ms),
        "--service-workers", str(getattr(args, "service_workers", 4)),
        "--timeout-ms", str(settings["timeout_ms"]),
        "--ack-timeout-ms", str(args.ack_timeout_ms),
        "--attempt-timeout-ms", str(settings["attempt_timeout_ms"]),
        "--health-interval-ms", str(settings["health_interval_ms"]),
        "--traffic-start-delay-s", str(args.traffic_start_delay_s),
        "--settle-seconds", str(args.settle_seconds),
        "--trace-profile", getattr(args, "trace_profile", "random-waypoint"),
        "--handoff-period-s", str(getattr(args, "handoff_period_s", 1.0)),
        "--ndnsf-strategy", args.ndnsf_strategy,
        "--seed", str(seed),
        "--lock-file", str(lock_path),
        "--output-dir", str(campaign_dir),
    ]
    if getattr(args, "grpc_no_health_routing", False):
        command.append("--grpc-no-health-routing")
    if getattr(args, "grpc_parallel", False):
        command.append("--grpc-parallel")
    if getattr(args, "block_network", False):
        command.append("--block-network")
    return command
    return command

def aggregate(records: list[dict], args) -> dict:
    complete = [record for record in records if record.get("status") == "complete"]
    by_condition = {}
    for condition in CONDITIONS:
        rows = [record for record in complete if record["condition"] == condition]
        systems = {}
        for system in SYSTEMS:
            system_rows = [row["systems"][system] for row in rows]
            total = sum(item["sent"] for item in system_rows)
            success = sum(item["success"] for item in system_rows)
            attempts = [item["attempts_per_request"] for item in system_rows]
            systems[system] = {
                "repetitions": len(system_rows),
                "requests": total,
                "success": success,
                "success_interval": wilson(success, total),
                "median_attempts_per_request": statistics.median(attempts) if attempts else None,
                "median_p95_ms": statistics.median(
                    [item["p95_ms"] for item in system_rows if item["p95_ms"] is not None]
                ) if any(item["p95_ms"] is not None for item in system_rows) else None,
                "median_parallel_cancellations": statistics.median(
                    [item.get("parallel_cancellations", 0) for item in system_rows]
                ) if system_rows else None,
                "median_server_extra_executions_per_request": statistics.median(
                    [item.get("server_extra_executions_per_request", 0)
                     for item in system_rows]
                ) if system_rows else None,
            }
        comparisons = {
            baseline: paired_bootstrap(rows, baseline, args.bootstrap_iterations,
                                       args.bootstrap_seed + index)
            for index, baseline in enumerate(("grpc", "nsc"))
        }
        ndnsf_attempts = systems["ndnsf"]["median_attempts_per_request"]
        baseline_attempts = [systems[name]["median_attempts_per_request"] for name in ("grpc", "nsc")]
        lower_baseline = min(item for item in baseline_attempts if item is not None) \
            if any(item is not None for item in baseline_attempts) else None
        by_condition[condition] = {
            "complete_repetitions": len(rows),
            "required_repetitions": args.min_repetitions,
            "systems": systems,
            "paired_success_difference": comparisons,
            "attempt_gate": {
                "ndnsf_median_attempts_per_request": ndnsf_attempts,
                "lower_baseline_median_attempts_per_request": lower_baseline,
                "passes": (
                    ndnsf_attempts is not None and lower_baseline is not None and
                    ndnsf_attempts <= 2.0 * lower_baseline
                ),
            },
        }
    stress = by_condition["stale-health"]
    trace_profile = getattr(args, "trace_profile", "single-active-handoff")
    enough = all(
        by_condition[name]["complete_repetitions"] >= args.min_repetitions
        for name in CONDITIONS
    )
    stress_advantage = all(
        stress["paired_success_difference"][baseline]["lower"] is not None and
        stress["paired_success_difference"][baseline]["lower"] >= 0.10
        for baseline in ("grpc", "nsc")
    )
    attempt_pass = stress["attempt_gate"]["passes"]
    if not enough:
        verdict = "INCONCLUSIVE_MISSING_CELL"
        supplementary_verdict = "INCONCLUSIVE_MISSING_CELL"
    elif stress_advantage and attempt_pass:
        # SC-005 is explicitly registered against the harsh one-provider-at-a-
        # time schedule.  A random-waypoint run can still demonstrate useful
        # redundant-coverage behavior, but it must never be promoted to the
        # formal mobility-superiority claim.
        verdict = ("NDNSF_MOBILITY_ADVANTAGE"
                   if trace_profile == "single-active-handoff"
                   else "NO_DEMONSTRATED_ADVANTAGE")
        supplementary_verdict = "NDNSF_REDUNDANT_COVERAGE_ADVANTAGE"
    else:
        verdict = "NO_DEMONSTRATED_ADVANTAGE"
        supplementary_verdict = "NO_DEMONSTRATED_REDUNDANT_COVERAGE_ADVANTAGE"
    return {
        "schema": "ndnsf-four-provider-mobility-repetition-v1",
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "configuration": {
            "systems": list(SYSTEMS),
            "conditions": CONDITIONS,
            "duration_s": args.duration_s,
            "rate_rps": args.rate_rps,
            "processing_delay_ms": args.processing_delay_ms,
            "ack_timeout_ms": args.ack_timeout_ms,
            "ndnsf_strategy": args.ndnsf_strategy,
            "trace_profile": getattr(args, "trace_profile", "random-waypoint"),
            "handoff_period_s": getattr(args, "handoff_period_s", 1.0),
            "grpc_no_health_routing": getattr(args, "grpc_no_health_routing", False),
            "grpc_parallel": getattr(args, "grpc_parallel", False),
            "block_network": getattr(args, "block_network", False),
            "requested_seeds": args.seeds,
            "min_repetitions": args.min_repetitions,
            "bootstrap_iterations": args.bootstrap_iterations,
            "bootstrap_seed": args.bootstrap_seed,
            "min_free_gb": getattr(args, "min_free_gb", 0.0),
        },
        "claim_verdict": verdict,
        "supplementary_verdict": supplementary_verdict,
        "records": records,
        "by_condition": by_condition,
    }


def write_outputs(output_root: Path, report: dict) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    (output_root / "aggregate.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    with (output_root / "aggregate.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["condition", "system", "repetitions", "requests", "success",
                         "success_rate", "wilson_lower", "wilson_upper",
                         "median_attempts_per_request", "median_p95_ms",
                         "median_parallel_cancellations",
                         "median_server_extra_executions_per_request"])
        for condition, payload in report["by_condition"].items():
            for system, metrics in payload["systems"].items():
                interval = metrics["success_interval"]
                writer.writerow([
                    condition, system, metrics["repetitions"], metrics["requests"],
                    metrics["success"], interval["estimate"], interval["lower"],
                    interval["upper"], metrics["median_attempts_per_request"],
                    metrics["median_p95_ms"],
                    metrics["median_parallel_cancellations"],
                    metrics["median_server_extra_executions_per_request"],
                ])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", default="Experiments/WifiRouterMobilityReliability.py")
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--seeds", type=parse_ints, default=list(range(20, 30)))
    parser.add_argument("--conditions", default=",".join(CONDITIONS),
                        help="comma-separated registered conditions to run")
    parser.add_argument("--min-repetitions", type=int, default=10)
    parser.add_argument("--duration-s", type=int, default=60)
    parser.add_argument("--rate-rps", type=float, default=5.0)
    parser.add_argument("--processing-delay-ms", type=int, default=5)
    parser.add_argument(
        "--service-workers", type=int, default=4,
        help="matched per-provider service worker/handler count for NDNSF and gRPC")
    parser.add_argument("--ack-timeout-ms", type=int, default=200)
    parser.add_argument("--traffic-start-delay-s", type=float, default=2.0)
    parser.add_argument("--settle-seconds", type=int, default=5)
    parser.add_argument(
        "--trace-profile", choices=("random-waypoint", "single-active-handoff"),
        default="random-waypoint")
    parser.add_argument("--handoff-period-s", type=float, default=1.0)
    parser.add_argument(
        "--grpc-no-health-routing", action="store_true",
        help="use strict sequential gRPC rather than health-assisted routing")
    parser.add_argument(
        "--grpc-parallel", action="store_true",
        help="use the separately labelled first-success parallel gRPC diagnostic")
    parser.add_argument(
        "--block-network", action="store_true",
        help="apply interface-level packet drops to out-of-coverage providers")
    parser.add_argument("--ndnsf-strategy", choices=("first-responding", "all-selected", "random-selection"), default="first-responding")
    parser.add_argument("--lock-file", default="/tmp/ndnsf-four-provider-repetitions.lock")
    parser.add_argument("--bootstrap-iterations", type=int, default=20000)
    parser.add_argument("--bootstrap-seed", type=int, default=171)
    parser.add_argument(
        "--min-free-gb", type=float, default=5.0,
        help="minimum free space required before the campaign; "
             "set to 0 to disable the guard")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--aggregate-only", action="store_true",
        help="read existing campaign directories without launching MiniNDN")
    args = parser.parse_args()
    args.selected_conditions = [item.strip() for item in args.conditions.split(",") if item.strip()]
    if not args.selected_conditions or any(item not in CONDITIONS for item in args.selected_conditions):
        parser.error("conditions must be selected from: " + ", ".join(CONDITIONS))
    if len(set(args.selected_conditions)) != len(args.selected_conditions):
        parser.error("conditions must not contain duplicates")
    if args.dry_run and args.aggregate_only:
        parser.error("--dry-run and --aggregate-only are mutually exclusive")
    if args.duration_s <= 0 or args.rate_rps <= 0 or args.min_repetitions <= 0:
        parser.error("duration, rate, and min-repetitions must be positive")
    if args.service_workers <= 0:
        parser.error("--service-workers must be positive")
    if args.handoff_period_s <= 0 or not math.isfinite(args.handoff_period_s):
        parser.error("handoff-period-s must be finite and positive")
    if args.min_free_gb < 0 or not math.isfinite(args.min_free_gb):
        parser.error("min-free-gb must be finite and non-negative")
    if not args.dry_run and not args.aggregate_only and os.geteuid() != 0:
        parser.error("MiniNDN repetitions must run as root; use sudo -n")
    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    enforce_disk_budget(output_root, args.min_free_gb, "before campaign")
    driver_root = output_root / "_driver"
    driver_root.mkdir(parents=True, exist_ok=True)
    records = []
    for condition in args.selected_conditions:
        for seed in args.seeds:
            campaign_dir = output_root / f"{condition}-seed-{seed}"
            command = command_for(args, condition, seed, campaign_dir)
            campaign_dir.mkdir(parents=True, exist_ok=True)
            if args.aggregate_only:
                records.append(read_campaign(campaign_dir, condition, seed))
                continue
            command_path = driver_root / f"{condition}-seed-{seed}-command.json"
            command_path.write_text(json.dumps({"argv": command, "command": " ".join(command)}, indent=2) + "\n")
            if args.dry_run:
                print(" ".join(command))
                continue
            existing = read_campaign(campaign_dir, condition, seed)
            if existing.get("status") in {"complete", "failed", "incomplete", "invalid"}:
                records.append(existing)
                continue
            log_path = driver_root / f"{condition}-seed-{seed}.log"
            started = time.monotonic()
            with log_path.open("w") as log:
                log.write("COMMAND " + " ".join(command) + "\n")
                log.flush()
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
            record = read_campaign(campaign_dir, condition, seed)
            record.update({
                "driver_returncode": completed.returncode,
                "driver_elapsed_s": round(time.monotonic() - started, 3),
                "driver_log": str(log_path.resolve()),
            })
            records.append(record)
            print(json.dumps(record, sort_keys=True))
    if args.dry_run:
        return 0
    report = aggregate(records, args)
    write_outputs(output_root, report)
    print(json.dumps({
        "claim_verdict": report["claim_verdict"],
        "output_root": str(output_root),
        "complete_repetitions": {
            condition: value["complete_repetitions"]
            for condition, value in report["by_condition"].items()
        },
    }, indent=2, sort_keys=True))
    return 0 if report["claim_verdict"] != "INCONCLUSIVE_MISSING_CELL" else 2


if __name__ == "__main__":
    raise SystemExit(main())
