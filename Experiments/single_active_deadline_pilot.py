#!/usr/bin/env python3
"""Run the registered single-active deadline-constrained NDNSF pilot.

This wrapper deliberately keeps the registered protocol independent from the
older moderate/stale repetition driver.  It creates one replay trace per seed,
then runs NDNSF, strict sequential gRPC, and NSC against that exact trace.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "Experiments" / "WifiRouterMobilityReliability.py"
MIN_FREE_GIB = 20.0
REGISTRATION_CONTRACT = {
    "ndnsf": {
        "client_endpoint_registration_required": False,
        "client_discovery": "runtime NDN namespace forwarding and controller permission bootstrap",
        "endpoint_list_in_client_command": False,
    },
    "grpc": {
        "client_endpoint_registration_required": True,
        "client_discovery": "none; four static --target name=host:port entries",
        "endpoint_list_in_client_command": True,
    },
    "nsc": {
        "client_endpoint_registration_required": True,
        "client_discovery": "none; four static Provider prefixes in consumer argv",
        "endpoint_list_in_client_command": True,
    },
}
CONFIG = {
    "profile": "four-provider-multi-ap",
    "ap_layout": "multi-ap",
    "range_m": 75.0,
    "speed_mps": 8.0,
    "trace_profile": "single-active-handoff",
    "handoff_period_s": 2.0,
    "systems": ["ndnsf", "grpc", "nsc"],
    "duration_s": 60,
    "rate_rps": 5.0,
    "processing_delay_ms": 5,
    "service_workers": 4,
    "global_deadline_ms": 1500,
    "ack_timeout_ms": 500,
    "attempt_timeout_ms": 500,
    "health_interval_ms": 500,
    "traffic_start_delay_s": 4.0,
    "settle_seconds": 5,
    "ndnsf_strategy": "first-responding",
    "grpc_no_health_routing": True,
    "grpc_parallel": False,
    "block_network": True,
    "traffic_phase_tolerance_s": 0.05,
    "seeds": [30, 31, 32],
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
            f"refusing campaign: {free_gib:.2f} GiB free, "
            f"minimum is {MIN_FREE_GIB:.1f} GiB")
    return {
        "checked_path": str(path.parent.resolve()),
        "free_gib": round(free_gib, 3),
        "minimum_free_gib": MIN_FREE_GIB,
    }


def load_harness():
    spec = importlib.util.spec_from_file_location("mobility_harness", HARNESS)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load harness: {HARNESS}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.configure_profile(
        CONFIG["profile"], CONFIG["ap_layout"], CONFIG["speed_mps"])
    return module


def generate_and_validate_trace(module, seed: int, path: Path) -> dict:
    horizon = (
        CONFIG["traffic_start_delay_s"] + CONFIG["duration_s"] +
        CONFIG["global_deadline_ms"] / 1000.0 + 2.0)
    metadata = module.generate_mobility_trace(
        path,
        CONFIG["range_m"],
        seed,
        horizon,
        interval_s=0.1,
        profile=CONFIG["trace_profile"],
        handoff_period_s=CONFIG["handoff_period_s"],
    )
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    epochs = sorted({row["time_s"] for row in rows})
    counts = {
        epoch: sum(int(row["in_range"]) for row in rows if row["time_s"] == epoch)
        for epoch in epochs
    }
    invalid = {epoch: count for epoch, count in counts.items() if count != 1}
    if invalid:
        raise RuntimeError(
            f"seed {seed} trace violates exactly-one coverage: {list(invalid.items())[:5]}")
    return {
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
        "metadata": metadata,
        "epochs": len(epochs),
        "all_unreachable_epochs": sum(count == 0 for count in counts.values()),
        "exactly_one_epochs": sum(count == 1 for count in counts.values()),
    }


def command_for(system: str, seed: int, campaign_id: str,
                trace_path: Path, output_dir: Path) -> list[str]:
    command = [
        sys.executable,
        str(HARNESS),
        "--single-run",
        "--ranges", str(CONFIG["range_m"]),
        "--systems", system,
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
        "--trace-profile", str(CONFIG["trace_profile"]),
        "--handoff-period-s", str(CONFIG["handoff_period_s"]),
        "--ndnsf-strategy", str(CONFIG["ndnsf_strategy"]),
        "--seed", str(seed),
        "--campaign-id", campaign_id,
        "--output-dir", str(output_dir),
        "--profile", str(CONFIG["profile"]),
        "--ap-layout", str(CONFIG["ap_layout"]),
        "--speed-mps", str(CONFIG["speed_mps"]),
        "--block-network",
        "--trace-replay", str(trace_path),
    ]
    if system in {"grpc", "nsc"}:
        command.append("--formal-cell")
    if system == "grpc":
        command.append("--grpc-no-health-routing")
    return command


def run_seed(module, root: Path, seed: int) -> dict:
    seed_root = root / f"seed-{seed}"
    traces = seed_root / "traces"
    traces.mkdir(parents=True, exist_ok=True)
    trace = traces / "single_active.csv"
    trace_info = generate_and_validate_trace(module, seed, trace)
    campaign_id = f"single-active-deadline-seed-{seed}"
    record = {
        "seed": seed,
        "campaign_id": campaign_id,
        "trace": trace_info,
        "registration_contract": REGISTRATION_CONTRACT,
        "systems": {},
        "status": "running",
    }
    (seed_root / "seed-manifest.json").write_text(
        json.dumps({"config": CONFIG, **record}, indent=2, sort_keys=True) + "\n")
    for system in CONFIG["systems"]:
        output_dir = seed_root / "cells" / system
        output_dir.mkdir(parents=True, exist_ok=True)
        command = command_for(system, seed, campaign_id, trace, output_dir)
        cell_manifest = {
            "schema": "ndnsf-single-active-deadline-cell-v1",
            "campaign_id": campaign_id,
            "seed": seed,
            "system": system,
            "trace_path": str(trace.resolve()),
            "trace_sha256": trace_info["sha256"],
            "registration_contract": REGISTRATION_CONTRACT[system],
            "command": command,
        }
        (output_dir / "cell-manifest.json").write_text(
            json.dumps(cell_manifest, indent=2, sort_keys=True) + "\n")
        log_path = seed_root / f"{system}.driver.log"
        started = time.monotonic()
        with log_path.open("w", encoding="utf-8") as log:
            log.write("COMMAND " + " ".join(command) + "\n")
            log.flush()
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                       check=False)
        summary_path = output_dir / "summary.json"
        system_record = {
            "command": command,
            "returncode": completed.returncode,
            "elapsed_s": round(time.monotonic() - started, 3),
            "summary_file": str(summary_path.resolve()),
        }
        if summary_path.is_file():
            summary = json.loads(summary_path.read_text())
            summaries = summary.get("summaries", [])
            if len(summaries) == 1:
                system_record["summary"] = summaries[0]
                if summaries[0].get("trace_source") != str(trace.resolve()):
                    system_record["trace_source_mismatch"] = True
                    record["status"] = "failed"
                if summaries[0].get("sent") != CONFIG["duration_s"] * CONFIG["rate_rps"]:
                    system_record["request_count_mismatch"] = True
                    record["status"] = "failed"
        if json.loads((output_dir / "cell-manifest.json").read_text()) != cell_manifest:
            system_record["cell_manifest_mismatch"] = True
            record["status"] = "failed"
        record["systems"][system] = system_record
        if (completed.returncode != 0 or "summary" not in system_record or
                system_record.get("trace_source_mismatch") or
                system_record.get("request_count_mismatch") or
                system_record.get("cell_manifest_mismatch")):
            record["status"] = "failed"
            break
    else:
        record["status"] = "complete"
    (seed_root / "seed-manifest.json").write_text(
        json.dumps({"config": CONFIG, **record}, indent=2, sort_keys=True) + "\n")
    return record


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(probability * len(ordered) + 0.999999) - 1))
    return ordered[index]


def bootstrap_differences(records: list[dict], baseline: str) -> dict:
    differences = [
        record["systems"]["ndnsf"]["summary"]["success"] /
        record["systems"]["ndnsf"]["summary"]["sent"] -
        record["systems"][baseline]["summary"]["success"] /
        record["systems"][baseline]["summary"]["sent"]
        for record in records
    ]
    rng = random.Random(171)
    samples = [
        sum(differences[rng.randrange(len(differences))] for _ in differences) /
        len(differences)
        for _ in range(20000)
    ]
    return {
        "per_seed": differences,
        "mean": sum(differences) / len(differences),
        "lower": percentile(samples, 0.025),
        "upper": percentile(samples, 0.975),
    }


def aggregate(records: list[dict]) -> dict:
    complete = [record for record in records if record["status"] == "complete"]
    systems = {}
    for system in CONFIG["systems"]:
        summaries = [record["systems"][system]["summary"] for record in complete]
        systems[system] = {
            "requests": sum(int(item["sent"]) for item in summaries),
            "success": sum(int(item["success"]) for item in summaries),
            "success_rate": (
                sum(int(item["success"]) for item in summaries) /
                sum(int(item["sent"]) for item in summaries)
                if summaries else None
            ),
            "p95_ms": [item.get("p95_ms") for item in summaries],
            "attempts": [item.get("attempts") for item in summaries],
            "failovers": [item.get("failovers") for item in summaries],
            "provider_executions": [
                item.get("provider_execution_counts", item.get("provider_attempts"))
                for item in summaries
            ],
        }
    comparisons = ({
        baseline: bootstrap_differences(complete, baseline)
        for baseline in ("grpc", "nsc")
    } if complete else {"grpc": None, "nsc": None})
    valid_traces = all(
        record["trace"]["all_unreachable_epochs"] == 0 and
        record["trace"]["exactly_one_epochs"] == record["trace"]["epochs"]
        for record in complete
    )
    claim = (
        len(complete) == len(CONFIG["seeds"])
        and valid_traces
        and all(comparison is not None and comparison["lower"] > 0
                for comparison in comparisons.values())
    )
    return {
        "schema": "ndnsf-single-active-deadline-pilot-v1",
        "config": CONFIG,
        "complete_seeds": [record["seed"] for record in complete],
        "systems": systems,
        "paired_success_difference": comparisons,
        "claim_verdict": "NDNSF_SINGLE_ACTIVE_DEADLINE_ADVANTAGE" if claim
        else "NO_DEMONSTRATED_ADVANTAGE",
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--seeds", default=",".join(str(seed) for seed in CONFIG["seeds"]))
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    seeds = [int(item) for item in args.seeds.split(",") if item.strip()]
    if seeds != CONFIG["seeds"]:
        raise SystemExit(f"registered seeds are fixed: {CONFIG['seeds']}")
    root = Path(args.output_root).resolve()
    if root.exists() and any(root.iterdir()) and not args.dry_run:
        raise SystemExit(f"output root is not empty: {root}")
    root.mkdir(parents=True, exist_ok=True)
    disk = disk_preflight(root)
    registration = {
        "schema": "ndnsf-single-active-deadline-registration-v1",
        "config": CONFIG,
        "harness": str(HARNESS),
        "harness_sha256": sha256_file(HARNESS),
        "wrapper_sha256": sha256_file(Path(__file__).resolve()),
        "disk_preflight": disk,
        "registration_contract": REGISTRATION_CONTRACT,
    }
    (root / "registration.json").write_text(
        json.dumps(registration, indent=2, sort_keys=True) + "\n")
    if args.dry_run:
        print(json.dumps(registration, indent=2, sort_keys=True))
        return 0
    module = load_harness()
    records = []
    for seed in seeds:
        print(f"PILOT_SEED_START seed={seed}", flush=True)
        record = run_seed(module, root, seed)
        records.append(record)
        print(json.dumps({"seed": seed, "status": record["status"]}, sort_keys=True), flush=True)
        if record["status"] != "complete":
            break
    report = aggregate(records)
    (root / "aggregate.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"claim_verdict": report["claim_verdict"],
                      "complete_seeds": report["complete_seeds"]}, sort_keys=True))
    complete = len(report["complete_seeds"]) == len(CONFIG["seeds"])
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
