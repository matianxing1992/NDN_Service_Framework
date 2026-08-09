#!/usr/bin/env python3
"""Registered one-AP, four-Provider coverage/speed mobility pilot.

This campaign is intentionally separate from the superseded multi-AP and
single-active deadline pilots.  It keeps one physical AP and varies the
declared coverage radius and deterministic drone speed.  The fair primary
comparison uses sequential gRPC failover with no proactive health oracle;
the custom application-level health RPC is an explicit sensitivity condition.

The default range/speed matrix remains the registered 50/100 m, 2/15 m/s
primary pilot.  The 75/150 m ranges and 5/10 m/s speeds are explicit sensitivity
extensions and are selected only when named with ``--ranges``/``--speeds``.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "Experiments" / "WifiRouterMobilityReliability.py"
MIN_FREE_GIB = 20.0
DEFAULT_PRIMARY_RANGES_M = (50.0, 100.0)
DEFAULT_PRIMARY_SPEEDS_MPS = (2.0, 15.0)
REGISTERED_SPEEDS_MPS = (2.0, 5.0, 10.0, 15.0)
DEFAULT_SEEDS = (40, 41, 42)
CONFIG = {
    "profile": "four-provider-single-ap",
    "ap_layout": "single",
    "providers": ["ucla", "wustl", "uiuc", "arizona"],
    "ranges_m": list(DEFAULT_PRIMARY_RANGES_M),
    "registered_ranges_m": [35.0, 40.0, 50.0, 70.0, 75.0, 80.0, 90.0, 100.0, 110.0, 120.0, 150.0],
    "speeds_mps": list(DEFAULT_PRIMARY_SPEEDS_MPS),
    "systems": ["ndnsf", "grpc", "nsc"],
    "single_provider": "ucla",
    "trace_profile": "random-waypoint",
    "duration_s": 60,
    "rate_rps": 5.0,
    "processing_delay_ms": 5,
    "service_workers": 4,
    "global_deadline_ms": 5000,
    "ack_timeout_ms": 1000,
    "attempt_timeout_ms": 1000,
    "health_interval_ms": 1000,
    "traffic_start_delay_s": 4.0,
    "settle_seconds": 5,
    "ndnsf_strategy": "first-responding",
    "mobility_warmup_s": 0.0,
    "ndnsf_response_retry": False,
    "grpc_no_health_routing": True,
    "grpc_health_oracle": "disabled-primary",
    "grpc_health_oracle_implementation": (
        "NDNSFBaseline/Health application RPC; not grpc.health.v1"),
    "grpc_parallel": False,
    "block_network": True,
    "traffic_phase_tolerance_s": 0.05,
    "timeout_condition": "primary-1s-attempt-5s-global",
    "ndnsf_first_responding_semantics": (
        "ackTimeoutMs does not delay first successful ACK selection; "
        "FirstResponding publishes selection immediately"),
    "seeds": list(DEFAULT_SEEDS),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def disk_preflight(path: Path) -> dict:
    usage = shutil.disk_usage(path.parent)
    free_gib = usage.free / (1024 ** 3)
    if free_gib < MIN_FREE_GIB:
        raise RuntimeError(
            f"refusing campaign: {free_gib:.2f} GiB free, minimum is "
            f"{MIN_FREE_GIB:.1f} GiB")
    return {"path": str(path.parent.resolve()), "free_gib": round(free_gib, 3),
            "minimum_free_gib": MIN_FREE_GIB}


def load_harness(speed: float):
    spec = importlib.util.spec_from_file_location("mobility_harness", HARNESS)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load harness: {HARNESS}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.configure_profile(CONFIG["profile"], CONFIG["ap_layout"], speed)
    return module


def coverage_metrics(rows: list[dict], *, start_s: float | None = None,
                     duration_s: float | None = None) -> dict:
    if (start_s is None) != (duration_s is None):
        raise ValueError("start_s and duration_s must be provided together")
    end_s = None if start_s is None else start_s + duration_s
    counts: dict[float, int] = {}
    for row in rows:
        epoch = float(row["time_s"])
        if start_s is not None and not (start_s <= epoch < end_s):
            continue
        counts[epoch] = counts.get(epoch, 0) + int(row["in_range"])
    if not counts:
        raise ValueError("coverage window contains no trace epochs")
    values = list(counts.values())
    return {
        "epochs": len(values),
        "start_s": start_s,
        "end_s": end_s,
        "at_least_one_fraction": sum(count > 0 for count in values) / len(values),
        "all_unreachable_fraction": sum(count == 0 for count in values) / len(values),
        "at_least_two_fraction": sum(count >= 2 for count in values) / len(values),
        "max_reachable": max(values),
    }


def trace_for(module, seed: int, speed: float, ap_range: float, path: Path) -> dict:
    horizon = (CONFIG["traffic_start_delay_s"] + CONFIG["duration_s"] +
               CONFIG["global_deadline_ms"] / 1000.0 + 2.0)
    metadata = module.generate_mobility_trace(
        path, ap_range, seed, horizon, interval_s=0.1,
        profile=CONFIG["trace_profile"],
        mobility_warmup_s=CONFIG["mobility_warmup_s"],
        measurement_start_s=float(CONFIG["traffic_start_delay_s"]),
        measurement_duration_s=float(CONFIG["duration_s"]))
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    whole_trace = coverage_metrics(rows)
    measurement_window = coverage_metrics(
        rows,
        start_s=float(CONFIG["traffic_start_delay_s"]),
        duration_s=float(CONFIG["duration_s"]),
    )
    return {
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
        "metadata": metadata,
        **whole_trace,
        "measurement_window": measurement_window,
    }


def condition_id(ap_range: float, speed: float) -> str:
    return f"range-{int(ap_range)}-speed-{str(speed).replace('.', 'p')}"


def parse_registered_values(raw: str, registered: list[float], label: str) -> list[float]:
    if not raw:
        return list(registered)
    try:
        values = [float(item.strip()) for item in raw.split(",") if item.strip()]
    except ValueError as error:
        raise SystemExit(f"invalid {label} list: {raw}") from error
    if not values or any(value not in registered for value in values):
        raise SystemExit(
            f"{label} must be a subset of registered values {registered}; got {values}")
    return values


def traffic_phase_matches(summary: dict) -> bool:
    try:
        actual = float(summary["traffic_launch_offset_s"])
    except (KeyError, TypeError, ValueError):
        return False
    return (
        math.isfinite(actual) and
        abs(actual - float(CONFIG["traffic_start_delay_s"])) <=
        float(CONFIG["traffic_phase_tolerance_s"])
    )


def parse_seed_values(raw: str) -> list[int]:
    if not raw:
        return list(DEFAULT_SEEDS)
    try:
        values = [int(item.strip()) for item in raw.split(",") if item.strip()]
    except ValueError as error:
        raise SystemExit(f"invalid seeds list: {raw}") from error
    if not values or len(set(values)) != len(values):
        raise SystemExit("seeds must be a non-empty list without duplicates")
    return values


def apply_timeout_overrides(global_deadline_ms=None, attempt_timeout_ms=None,
                            ack_timeout_ms=None):
    """Apply an explicitly paired timeout condition to the campaign config."""
    values = {
        "global_deadline_ms": global_deadline_ms,
        "attempt_timeout_ms": attempt_timeout_ms,
        "ack_timeout_ms": ack_timeout_ms,
    }
    for key, value in values.items():
        if value is None:
            continue
        if isinstance(value, bool) or int(value) <= 0:
            raise SystemExit(f"{key} must be a positive integer")
        CONFIG[key] = int(value)
    if (CONFIG["attempt_timeout_ms"] == 1000 and
            CONFIG["ack_timeout_ms"] == 1000 and
            CONFIG["global_deadline_ms"] == 5000):
        CONFIG["timeout_condition"] = "primary-1s-attempt-5s-global"
    else:
        CONFIG["timeout_condition"] = (
            f"sensitivity-{CONFIG['attempt_timeout_ms']}ms-attempt-"
            f"{CONFIG['global_deadline_ms']}ms-global-"
            f"{CONFIG['ack_timeout_ms']}ms-ack")


def command_for(system: str, seed: int, campaign_id: str, trace: Path,
                output_dir: Path, ap_range: float, speed: float) -> list[str]:
    harness_system = {
        "grpc-single": "grpc",
        "nsc-single": "nsc",
    }.get(system, system)
    single_provider = system in {"grpc-single", "nsc-single"}
    command = [
        sys.executable, str(HARNESS), "--single-run",
        "--ranges", str(ap_range), "--systems", harness_system,
        "--duration-s", str(CONFIG["duration_s"]),
        "--rate-rps", str(CONFIG["rate_rps"]),
        "--processing-delay-ms", str(CONFIG["processing_delay_ms"]),
        "--service-workers", str(CONFIG["service_workers"]),
        "--timeout-ms", str(CONFIG["global_deadline_ms"]),
        "--ack-timeout-ms", str(CONFIG["ack_timeout_ms"]),
        "--attempt-timeout-ms", str(CONFIG["attempt_timeout_ms"]),
        "--health-interval-ms", str(CONFIG["health_interval_ms"]),
        "--traffic-start-delay-s", str(CONFIG["traffic_start_delay_s"]),
        "--settle-seconds", str(CONFIG["settle_seconds"]),
        "--trace-profile", CONFIG["trace_profile"],
        "--handoff-period-s", "1.0",
        "--ndnsf-strategy", CONFIG["ndnsf_strategy"],
        "--seed", str(seed), "--campaign-id", campaign_id,
        "--output-dir", str(output_dir), "--profile", CONFIG["profile"],
        "--ap-layout", CONFIG["ap_layout"], "--speed-mps", str(speed),
        "--block-network", "--trace-replay", str(trace),
        # Keep the harness lock inside this campaign so a stale root-owned
        # repository lock cannot prevent a reproducible holdout run.
        "--lock-file", str(output_dir.parents[2] / ".campaign.lock"),
    ]
    # The harness's formal-cell contract is the historical 60 s / 300-request
    # condition.  Longer sensitivity windows must remain explicitly labelled
    # as sensitivity runs rather than carrying that legacy marker.
    if (system in {"grpc", "nsc", "grpc-single", "nsc-single"} and
            CONFIG["duration_s"] == 60):
        command.append("--formal-cell")
    if single_provider:
        command.extend(("--provider-scope", CONFIG["single_provider"]))
    # Make the health policy explicit because the single-AP harness profile
    # defaults to the fair no-oracle mode.
    if system in {"grpc", "grpc-single"}:
        command.append(
            "--grpc-no-health-routing"
            if CONFIG["grpc_no_health_routing"] else "--grpc-health-routing")
    if system == "ndnsf" and CONFIG["ndnsf_response_retry"]:
        command.append("--ndnsf-response-retry")
    return command


def ndnsf_runtime_identity() -> dict:
    build_dir = Path(os.environ.get(
        "NDNSF_MOBILITY_BUILD_DIR", str(ROOT / "build"))).expanduser().resolve()
    runtime_dir = os.environ.get("NDNSF_MOBILITY_RUNTIME_LIB_DIR", "")
    svs_path = (Path(runtime_dir).expanduser().resolve() / "libndn-svs.so.0.1.0"
                if runtime_dir else None)
    framework_path = build_dir / "libndn-service-framework.so"
    return {
        "build_dir": str(build_dir),
        "framework_library": str(framework_path),
        "framework_library_sha256": (
            sha256_file(framework_path) if framework_path.is_file() else None),
        "runtime_library_dir": str(runtime_dir) if runtime_dir else None,
        "svs_library": str(svs_path) if svs_path else None,
        "svs_library_sha256": (
            sha256_file(svs_path)
            if svs_path and svs_path.is_file() else None),
    }


def run_cell(module, root: Path, seed: int, ap_range: float, speed: float,
             trace_info: dict, system: str, campaign_id: str) -> dict:
    cid = condition_id(ap_range, speed)
    cell_root = root / f"seed-{seed}" / cid / system
    cell_root.mkdir(parents=True, exist_ok=True)
    trace = Path(trace_info["path"])
    command = command_for(system, seed, campaign_id, trace, cell_root, ap_range, speed)
    trace_metrics = {
        "whole_trace": {
            key: trace_info[key]
            for key in (
                "epochs", "start_s", "end_s", "at_least_one_fraction",
                "all_unreachable_fraction", "at_least_two_fraction",
                "max_reachable")
        },
        "measurement_window": trace_info["measurement_window"],
    }
    manifest = {
        "schema": "ndnsf-single-ap-range-speed-cell-v1",
        "campaign_id": campaign_id, "seed": seed, "condition": cid,
        "system": system, "range_m": ap_range, "speed_mps": speed,
        "trace_sha256": trace_info["sha256"], "trace_path": str(trace),
        "trace_metrics": trace_metrics,
        "traffic_start_delay_s": CONFIG["traffic_start_delay_s"],
        "timeout_condition": CONFIG["timeout_condition"],
        "global_deadline_ms": CONFIG["global_deadline_ms"],
        "attempt_timeout_ms": CONFIG["attempt_timeout_ms"],
        "ack_timeout_ms": CONFIG["ack_timeout_ms"],
        "mobility_warmup_s": CONFIG["mobility_warmup_s"],
        "ndnsf_response_retry": (
            CONFIG["ndnsf_response_retry"] if system == "ndnsf" else None),
        "admission_control": "disabled",
        "health_oracle": CONFIG["grpc_health_oracle"] if system == "grpc" else "not-applicable",
        "ndnsf_runtime": ndnsf_runtime_identity() if system == "ndnsf" else None,
        "command": command,
    }
    (cell_root / "cell-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    log_path = root / f"seed-{seed}" / cid / f"{system}.driver.log"
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        log.write("COMMAND " + " ".join(command) + "\n")
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                   check=False)
    summary_path = cell_root / "summary.json"
    result = {
        "seed": seed, "condition": cid, "system": system,
        "range_m": ap_range, "speed_mps": speed,
        "returncode": completed.returncode,
        "elapsed_s": round(time.monotonic() - started, 3),
        "summary_file": str(summary_path.resolve()),
        "cell_manifest": str((cell_root / "cell-manifest.json").resolve()),
        "trace_metrics": trace_metrics,
    }
    if summary_path.is_file():
        envelope = json.loads(summary_path.read_text())
        summaries = envelope.get("summaries", [])
        if len(summaries) == 1:
            result["summary"] = summaries[0]
    if "summary" in result:
        summary = result["summary"]
        result["trace_source_match"] = summary.get("trace_source") == str(trace.resolve())
        result["request_count_match"] = summary.get("sent") == CONFIG["duration_s"] * CONFIG["rate_rps"]
        result["traffic_phase_match"] = traffic_phase_matches(summary)
    result["manifest_match"] = json.loads((cell_root / "cell-manifest.json").read_text()) == manifest
    result["status"] = "complete" if (
        completed.returncode == 0 and "summary" in result and
        result.get("trace_source_match") and result.get("request_count_match") and
        result.get("traffic_phase_match") and
        result["manifest_match"]
    ) else "failed"
    return result


def bootstrap(values: list[float]) -> dict:
    if not values:
        return {"mean": None, "lower": None, "upper": None, "per_seed": []}
    rng = random.Random(171)
    samples = [sum(values[rng.randrange(len(values))] for _ in values) / len(values)
               for _ in range(20000)]
    ordered = sorted(samples)
    return {"mean": sum(values) / len(values),
            "lower": ordered[int(0.025 * len(ordered))],
            "upper": ordered[int(0.975 * len(ordered))],
            "per_seed": values}


def aggregate(records: list[dict]) -> dict:
    complete = [r for r in records if r["status"] == "complete"]
    by_condition = {}
    for cid in sorted({r["condition"] for r in records}):
        group = [r for r in complete if r["condition"] == cid]
        by_system = {}
        for system in CONFIG["systems"]:
            rows = [r["summary"] for r in group if r["system"] == system]
            by_system[system] = {
                "requests": sum(int(x["sent"]) for x in rows),
                "success": sum(int(x["success"]) for x in rows),
                "success_rate": (sum(int(x["success"]) for x in rows) /
                                 sum(int(x["sent"]) for x in rows) if rows else None),
                "p50_ms": [x.get("p50_ms") for x in rows],
                "p95_ms": [x.get("p95_ms") for x in rows],
                "mean_ms": [x.get("mean_ms") for x in rows],
                "attempts": [x.get("attempts") for x in rows],
                "failovers": [x.get("failovers") for x in rows],
                "health_directed_selections": [x.get("health_directed_selections") for x in rows],
                "provider_executions": [x.get("provider_executions", x.get("handler_executions_observed")) for x in rows],
                "provider_scope": [x.get("provider_scope") for x in rows],
            }
            total_success = sum(int(x["success"]) for x in rows)
            weighted_latency = sum(
                float(x.get("mean_ms", 0.0)) * int(x["success"])
                for x in rows)
            by_system[system]["mean_success_latency_ms"] = (
                weighted_latency / total_success if total_success else None)
            by_system[system]["mean_attempts_per_request"] = (
                None if system == "ndnsf" else (
                    sum(int(x.get("attempts", 0)) for x in rows) /
                    by_system[system]["requests"] if rows else None))
            by_system[system]["mean_failovers_per_request"] = (
                0.0 if system == "ndnsf" else (
                    sum(int(x.get("failovers", 0)) for x in rows) /
                    by_system[system]["requests"] if rows else None))
            by_system[system]["mean_provider_executions_per_request"] = (
                sum(int(x.get("provider_executions", x.get("attempts", 0)) or 0)
                    for x in rows) /
                by_system[system]["requests"] if rows else None)
        nd = {r["seed"]: r["summary"]["success"] / r["summary"]["sent"]
              for r in group if r["system"] == "ndnsf"}
        comparisons = {}
        for baseline in ("grpc", "nsc", "grpc-single", "nsc-single"):
            if baseline not in CONFIG["systems"]:
                continue
            base = {r["seed"]: r["summary"]["success"] / r["summary"]["sent"]
                    for r in group if r["system"] == baseline}
            keys = sorted(set(nd) & set(base))
            comparisons[baseline] = bootstrap([nd[k] - base[k] for k in keys])
        trace_metrics_by_seed = {}
        for row in group:
            trace_metrics_by_seed[str(row["seed"])] = row["trace_metrics"]
        by_condition[cid] = {
            "systems": by_system,
            "paired_success_difference": comparisons,
            "complete_seeds": sorted({r["seed"] for r in group}),
            "trace_metrics_by_seed": trace_metrics_by_seed,
        }
    return {
        "schema": "ndnsf-single-ap-range-speed-pilot-v1", "config": CONFIG,
        "complete_cells": len(complete), "total_cells": len(records),
        "by_condition": by_condition,
        "claim_verdict": "DESCRIPTIVE_RANGE_SPEED_MATRIX_ONLY",
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--ranges", default="",
        help="optional comma-separated subset of registered coverage radii")
    parser.add_argument(
        "--speeds", default="",
        help="optional comma-separated subset of registered speeds")
    parser.add_argument(
        "--seeds", default="",
        help="optional comma-separated seed subset; default is 40,41,42")
    parser.add_argument(
        "--systems", default="",
        help="optional comma-separated subset of registered systems")
    parser.add_argument(
        "--grpc-health-oracle", choices=("disabled", "enabled"),
        default="disabled",
        help=("explicitly enable the experiment's custom application-level "
              "health RPC; default is disabled for the fair baseline"),
    )
    parser.add_argument(
        "--include-single-provider-baselines", action="store_true",
        help="add fixed-provider gRPC-1 and NSC-1 no-failover controls",
    )
    parser.add_argument(
        "--global-deadline-ms", type=int, default=None,
        help="override the common logical deadline; default is registered 5000 ms",
    )
    parser.add_argument(
        "--attempt-timeout-ms", type=int, default=None,
        help="override gRPC/NSC per-attempt timeout; default is registered 1000 ms",
    )
    parser.add_argument(
        "--ack-timeout-ms", type=int, default=None,
        help=("override NDNSF ACK timeout; with FirstResponding this does not "
              "delay first successful ACK selection"),
    )
    parser.add_argument(
        "--duration-s", type=int, default=None,
        help="override the measured window duration; default is the registered 60 s",
    )
    parser.add_argument(
        "--mobility-warmup-s", type=float, default=0.0,
        help=("advance deterministic RandomWaypoint state before trace time zero; "
              "does not add wall-clock delay"),
    )
    parser.add_argument(
        "--ndnsf-response-retry", action="store_true",
        help="enable bounded Response-timeout reselection for NDNSF only",
    )
    args = parser.parse_args()
    if args.grpc_health_oracle == "enabled":
        CONFIG["grpc_no_health_routing"] = False
        CONFIG["grpc_health_oracle"] = "enabled-explicit-application-rpc"
    else:
        CONFIG["grpc_no_health_routing"] = True
        CONFIG["grpc_health_oracle"] = "disabled-primary"
    apply_timeout_overrides(
        args.global_deadline_ms, args.attempt_timeout_ms, args.ack_timeout_ms)
    if args.duration_s is not None:
        if args.duration_s <= 0:
            raise SystemExit("duration-s must be a positive integer")
        CONFIG["duration_s"] = int(args.duration_s)
    if not math.isfinite(args.mobility_warmup_s) or args.mobility_warmup_s < 0:
        raise SystemExit("mobility-warmup-s must be finite and non-negative")
    CONFIG["mobility_warmup_s"] = float(args.mobility_warmup_s)
    CONFIG["ndnsf_response_retry"] = bool(args.ndnsf_response_retry)
    range_candidates = (
        list(CONFIG["registered_ranges_m"])
        if args.ranges else list(DEFAULT_PRIMARY_RANGES_M))
    CONFIG["ranges_m"] = parse_registered_values(
        args.ranges, range_candidates, "ranges")
    CONFIG["speeds_mps"] = (
        list(DEFAULT_PRIMARY_SPEEDS_MPS)
        if not args.speeds else
        parse_registered_values(args.speeds, list(REGISTERED_SPEEDS_MPS), "speeds")
    )
    CONFIG["seeds"] = parse_seed_values(args.seeds)
    CONFIG["systems"] = ["ndnsf", "grpc", "nsc"]
    if args.include_single_provider_baselines:
        CONFIG["systems"].extend(("grpc-single", "nsc-single"))
    if args.systems:
        requested_systems = [item.strip() for item in args.systems.split(",")
                             if item.strip()]
        if (not requested_systems or
                any(item not in {"ndnsf", "grpc", "nsc", "grpc-single", "nsc-single"}
                    for item in requested_systems)):
            raise SystemExit("--systems contains an unsupported system")
        CONFIG["systems"] = requested_systems
    root = Path(args.output_root).resolve()
    if root.exists() and any(root.iterdir()) and not args.dry_run:
        raise SystemExit(f"output root is not empty: {root}")
    root.mkdir(parents=True, exist_ok=True)
    registration = {
        "schema": "ndnsf-single-ap-range-speed-registration-v1",
        "config": CONFIG, "harness": str(HARNESS),
        "harness_sha256": sha256_file(HARNESS),
        "wrapper_sha256": sha256_file(Path(__file__).resolve()),
        "disk_preflight": disk_preflight(root),
        "registration_contract": {
            "ndnsf": "no static endpoint list; runtime NDN namespace plus permission/token bootstrap",
            "grpc": (
                "four static targets; sequential one-at-a-time failover is the "
                "primary; custom health RPC is explicit opt-in only"),
            "nsc": "four static Provider prefixes; sequential 1 s attempt timeout",
            "grpc-single": (
                "one fixed gRPC target; single-provider mode forbids failover"),
            "nsc-single": (
                "one fixed NSC Provider prefix; no retry/failover to other Providers"),
        },
    }
    (root / "registration.json").write_text(
        json.dumps(registration, indent=2, sort_keys=True) + "\n")
    if args.dry_run:
        print(json.dumps(registration, indent=2, sort_keys=True))
        return 0

    records = []
    for seed in CONFIG["seeds"]:
        for speed in CONFIG["speeds_mps"]:
            module = load_harness(speed)
            for ap_range in CONFIG["ranges_m"]:
                condition = condition_id(ap_range, speed)
                trace_path = root / f"seed-{seed}" / condition / "trace.csv"
                trace_path.parent.mkdir(parents=True, exist_ok=True)
                trace_info = trace_for(module, seed, speed, ap_range, trace_path)
                trace_info["range_m"] = ap_range
                trace_info["speed_mps"] = speed
                (trace_path.parent / "trace-info.json").write_text(
                    json.dumps(trace_info, indent=2, sort_keys=True) + "\n")
                campaign_id = f"single-ap-range-speed-seed-{seed}-{condition}"
                for system in CONFIG["systems"]:
                    print(f"CELL_START seed={seed} condition={condition} system={system}", flush=True)
                    result = run_cell(module, root, seed, ap_range, speed,
                                      trace_info, system, campaign_id)
                    records.append(result)
                    (Path(result["cell_manifest"]).parent / "cell-result.json").write_text(
                        json.dumps(result, indent=2, sort_keys=True) + "\n")
                    checkpoint = aggregate(records)
                    (root / "aggregate.json").write_text(
                        json.dumps(checkpoint, indent=2, sort_keys=True) + "\n")
                    print(json.dumps({"seed": seed, "condition": condition,
                                      "system": system, "status": result["status"]}, sort_keys=True),
                          flush=True)
                    if result["status"] != "complete":
                        return 1
    report = aggregate(records)
    (root / "aggregate.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"complete_cells": report["complete_cells"],
                      "total_cells": report["total_cells"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
