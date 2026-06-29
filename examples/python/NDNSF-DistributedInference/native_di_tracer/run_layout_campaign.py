#!/usr/bin/env python3
"""Run repeated MiniNDN NativeTracer layout measurements."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import pwd
import statistics
import subprocess
import sys
import time
from typing import Any


REPO = Path(__file__).resolve().parents[4]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"

ASSIGNMENTS = [
    ("default", "shared-backbone-current"),
    ("single-provider", "single-provider-serial"),
]

ASSIGNMENT_CANDIDATES = {
    "auto": "planner-selected",
    "default": "shared-backbone-current",
    "single-provider": "single-provider-serial",
    "capacity-pool": "shared-backbone-current",
}


def percentile_nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        raise RuntimeError("cannot compute percentile of empty values")
    ordered = sorted(values)
    rank = max(1, math.ceil((percentile / 100.0) * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def stats(values: list[float]) -> dict[str, float | int]:
    if not values:
        raise RuntimeError("cannot summarize empty campaign")
    return {
        "count": len(values),
        "meanMs": round(statistics.mean(values), 3),
        "stddevMs": round(statistics.stdev(values), 3) if len(values) > 1 else 0.0,
        "p50Ms": round(statistics.median(values), 3),
        "p95Ms": round(percentile_nearest_rank(values, 95.0), 3),
        "minMs": round(min(values), 3),
        "maxMs": round(max(values), 3),
    }


def load_summary(run_dir: Path) -> dict[str, Any]:
    summary_path = run_dir / "summary.json"
    if not summary_path.exists():
        raise RuntimeError(f"run did not produce summary.json: {summary_path}")
    return json.loads(summary_path.read_text(encoding="utf-8"))


def timing_mean(values: list[float]) -> float:
    return round(statistics.mean(values), 3) if values else 0.0


def timing_max(values: list[float]) -> float:
    return round(max(values), 3) if values else 0.0


def parse_provider_timing(run_dir: Path) -> dict[str, float | int]:
    rows: list[dict[str, str]] = []
    for log_path in sorted((run_dir / "logs").glob("provider-serve-*.log")):
        for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "NDNSF_DI_PROVIDER_HANDLER_TIMING" not in line or " event=end " not in line:
                continue
            row: dict[str, str] = {}
            for part in line.split():
                if "=" not in part:
                    continue
                key, value = part.split("=", 1)
                row[key] = value
            rows.append(row)

    def values(column: str) -> list[float]:
        return [float(row[column]) for row in rows if column in row]

    queue_wait = values("queue_wait_ms")
    input_wait = values("input_fetch_wait_ms")
    runner_publish = values("runner_publish_ms")
    handler = values("handler_ms")
    total = values("total_ms")
    return {
        "providerTimingRows": len(rows),
        "providerQueueWaitMeanMs": timing_mean(queue_wait),
        "providerQueueWaitMaxMs": timing_max(queue_wait),
        "providerInputFetchWaitMeanMs": timing_mean(input_wait),
        "providerInputFetchWaitMaxMs": timing_max(input_wait),
        "providerRunnerPublishMeanMs": timing_mean(runner_publish),
        "providerRunnerPublishMaxMs": timing_max(runner_publish),
        "providerHandlerMeanMs": timing_mean(handler),
        "providerHandlerMaxMs": timing_max(handler),
        "providerTotalMeanMs": timing_mean(total),
        "providerTotalMaxMs": timing_max(total),
    }


def provider_from_log_path(log_path: Path) -> str:
    stem = log_path.stem
    marker = "--"
    if marker not in stem:
        return ""
    return "/" + stem.split(marker, 1)[1].replace("-", "/")


def parse_provider_allocation(run_dir: Path) -> dict[str, float | int | str]:
    counts: dict[tuple[str, str], int] = {}
    for log_path in sorted((run_dir / "logs").glob("provider-serve-*.log")):
        provider = provider_from_log_path(log_path)
        if not provider:
            continue
        for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "NDNSF_DI_PROVIDER_HANDLER_TIMING" not in line or " event=end " not in line:
                continue
            role = ""
            for part in line.split():
                if part.startswith("role="):
                    role = part.split("=", 1)[1]
                    break
            if not role:
                continue
            counts[(role, provider)] = counts.get((role, provider), 0) + 1

    by_role: dict[str, dict[str, int]] = {}
    for (role, provider), count in sorted(counts.items()):
        by_role.setdefault(role, {})[provider] = count

    backbone_counts = by_role.get("/Backbone", {})
    default_backbone = int(
        backbone_counts.get("/NDNSF/DI/Tracer/provider/backbone", 0))
    replica_backbone = int(
        backbone_counts.get("/NDNSF/DI/Tracer/provider/single", 0))
    total_backbone = default_backbone + replica_backbone
    replica_share = (
        round(replica_backbone / total_backbone, 6)
        if total_backbone > 0 else 0.0
    )
    return {
        "providerRoleExecutionRows": int(sum(counts.values())),
        "backboneDefaultExecutions": default_backbone,
        "backboneReplicaExecutions": replica_backbone,
        "backboneReplicaShare": replica_share,
        "roleExecutionByProviderJson": json.dumps(
            by_role, sort_keys=True, separators=(",", ":")),
    }


def parse_provider_capacity(run_dir: Path) -> dict[str, float | int]:
    rows: list[dict[str, str]] = []
    for log_path in sorted((run_dir / "logs").glob("provider-serve-*.log")):
        for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "NDNSF_DI_PROVIDER_CAPACITY" not in line:
                continue
            row: dict[str, str] = {}
            for part in line.split():
                if "=" not in part:
                    continue
                key, value = part.split("=", 1)
                row[key] = value
            rows.append(row)

    def values(column: str) -> list[float]:
        return [float(row[column]) for row in rows if column in row]

    pending = values("pending_work")
    ready_queue = values("ready_queue")
    waiting_inputs = values("waiting_inputs")
    active_workers = values("active_workers")
    idle_workers = values("idle_workers")
    workers = values("workers")
    before = [row for row in rows if row.get("event") == "before_submit"]
    after = [row for row in rows if row.get("event") == "after_complete"]
    return {
        "providerCapacityRows": len(rows),
        "providerCapacityBeforeSubmitRows": len(before),
        "providerCapacityAfterCompleteRows": len(after),
        "providerCapacityPendingMean": timing_mean(pending),
        "providerCapacityPendingMax": timing_max(pending),
        "providerCapacityReadyQueueMean": timing_mean(ready_queue),
        "providerCapacityReadyQueueMax": timing_max(ready_queue),
        "providerCapacityWaitingInputsMean": timing_mean(waiting_inputs),
        "providerCapacityWaitingInputsMax": timing_max(waiting_inputs),
        "providerCapacityActiveWorkersMean": timing_mean(active_workers),
        "providerCapacityActiveWorkersMax": timing_max(active_workers),
        "providerCapacityIdleWorkersMean": timing_mean(idle_workers),
        "providerCapacityWorkerCountMax": timing_max(workers),
    }


def parse_optimization_costs(summary: dict[str, Any]) -> dict[str, float]:
    optimization = summary.get("optimizationEvidence", {})
    evidence_path = Path(str(optimization.get("path", "")))
    if not evidence_path.exists():
        return {
            "selectedProviderReadyQueuePressureMs": 0.0,
            "recommendedProviderReadyQueuePressureMs": 0.0,
            "sharedProviderReadyQueuePressureMs": 0.0,
            "singleProviderReadyQueuePressureMs": 0.0,
        }
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    candidates = {
        str(item["id"]): item.get("cost", {})
        for item in evidence.get("candidates", [])
    }

    def pressure(candidate_id: str) -> float:
        return float(candidates.get(candidate_id, {}).get("providerReadyQueuePressureMs", 0.0))

    def cost_value(candidate_id: str, key: str) -> float:
        return float(candidates.get(candidate_id, {}).get(key, 0.0))

    selected_id = str(evidence.get("selectedCandidate", {}).get("id", ""))
    recommended_id = str(evidence.get("plannerRecommendedCandidate", {}).get("id", ""))
    return {
        "selectedProviderReadyQueuePressureMs": pressure(selected_id),
        "recommendedProviderReadyQueuePressureMs": pressure(recommended_id),
        "sharedProviderReadyQueuePressureMs": pressure("shared-backbone-current"),
        "singleProviderReadyQueuePressureMs": pressure("single-provider-serial"),
        "selectedProviderMaxUtilization": cost_value(selected_id, "providerMaxUtilization"),
        "selectedProviderCapacityQueuePressureMs": cost_value(
            selected_id, "providerCapacityQueuePressureMs"),
        "selectedDependencyByteRateMbps": cost_value(selected_id, "dependencyByteRateMbps"),
        "selectedDependencyMaxLinkUtilization": cost_value(
            selected_id, "dependencyMaxLinkUtilization"),
        "selectedDependencyRatePressureMs": cost_value(
            selected_id, "dependencyRatePressureMs"),
        "sharedProviderMaxUtilization": cost_value(
            "shared-backbone-current", "providerMaxUtilization"),
        "singleProviderMaxUtilization": cost_value(
            "single-provider-serial", "providerMaxUtilization"),
    }


def run_one(assignment: str,
            run_index: int,
            out_root: Path,
            provider_check_timeout: int,
            extra_args: list[str],
            activation_pad_bytes: int,
            role_execution_delay_ms: float,
            requests: int,
            concurrency: int,
            target_rps: float = 0.0,
            submission_spacing_ms: int = 0) -> dict[str, Any]:
    run_dir = out_root / assignment / f"run-{run_index:02d}"
    if run_dir.exists():
        subprocess.run(
            ["sudo", "-n", "rm", "-rf", str(run_dir)],
            cwd=str(REPO),
            check=True)
    user_site = ""
    sudo_user = os.environ.get("SUDO_USER") or pwd.getpwuid(os.getuid()).pw_name
    if sudo_user and sudo_user != "root":
        try:
            sudo_home = pwd.getpwnam(sudo_user).pw_dir
            user_site = str(
                Path(sudo_home) / ".local/lib" /
                f"python{sys.version_info.major}.{sys.version_info.minor}" /
                "site-packages")
        except KeyError:
            user_site = ""
    python_paths = [
        str(REPO / "pythonWrapper"),
        str(REPO / "NDNSF-DistributedInference"),
        user_site,
        os.environ.get("PYTHONPATH", ""),
    ]
    pythonpath = ":".join([
        path for path in python_paths
        if path
    ])
    ld_library_path = ":".join([
        str(REPO / "build"),
        os.environ.get("LD_LIBRARY_PATH", ""),
    ])
    ndnsf_env = [
        f"{key}={value}"
        for key, value in sorted(os.environ.items())
        if key.startswith("NDNSF_")
    ]
    env_prefix = [
        "env",
        f"PYTHONPATH={pythonpath}",
        f"LD_LIBRARY_PATH={ld_library_path}",
        *ndnsf_env,
    ]
    command = [
        *(["sudo", "-n"] if os.geteuid() != 0 else []),
        *env_prefix,
        "python3", str(HARNESS),
        "--full-network",
        "--assignment", assignment,
        "--out", str(run_dir),
        "--provider-check-timeout", str(provider_check_timeout),
        "--activation-pad-bytes", str(activation_pad_bytes),
        "--role-execution-delay-ms", str(role_execution_delay_ms),
        "--requests", str(requests),
        "--concurrency", str(concurrency),
        "--target-rps", str(target_rps),
        "--submission-spacing-ms", str(int(submission_spacing_ms)),
        *extra_args,
    ]
    print("RUN", " ".join(command), flush=True)
    cleanup_log = run_dir / "mininet-cleanup.log"
    run_dir.mkdir(parents=True, exist_ok=True)
    with cleanup_log.open("w", encoding="utf-8") as cleanup_output:
        subprocess.run(
            ["sudo", "-n", "mn", "-c"],
            cwd=str(REPO),
            stdout=cleanup_output,
            stderr=subprocess.STDOUT,
            check=False)
    time.sleep(1.0)
    subprocess.run(command, cwd=str(REPO), check=True)
    summary = load_summary(run_dir)
    if summary.get("status") != "SUCCESS":
        raise RuntimeError(f"run failed: {run_dir}")
    user = summary.get("userExecution", {})
    optimization = summary.get("optimizationEvidence", {})
    if user.get("status") != "executed":
        raise RuntimeError(f"user execution not executed: {run_dir}")
    if summary.get("dependencyExecution", {}).get("status") != "executed":
        raise RuntimeError(f"dependency execution not executed: {run_dir}")
    provider_timing = parse_provider_timing(run_dir)
    provider_capacity = parse_provider_capacity(run_dir)
    provider_allocation = parse_provider_allocation(run_dir)
    optimization_costs = parse_optimization_costs(summary)
    return {
        "assignment": assignment,
        "assignmentRequested": summary.get("assignmentRequested", assignment),
        "assignmentResolved": summary.get("assignmentResolved", summary.get("assignment", assignment)),
        "activationPadBytes": int(summary.get("activationPadBytes", activation_pad_bytes) or 0),
        "roleExecutionDelayMs": float(
            summary.get("roleExecutionDelayMs", role_execution_delay_ms) or 0.0),
        "requestCount": int(summary.get("requestCount", requests) or requests),
        "concurrency": int(summary.get("concurrency", concurrency) or concurrency),
        "targetRps": float(
            summary.get("optimizationEvidence", {}).get("targetRps", target_rps) or 0.0),
        "submissionSpacingMs": float(
            summary.get("submissionSpacingMs", submission_spacing_ms) or 0.0),
        "run": run_index,
        "resultDir": str(run_dir),
        "status": summary.get("status", ""),
        "runnerMode": summary.get("runnerMode", ""),
        "selectedCandidate": optimization.get("selectedCandidate", ""),
        "runtimeCandidate": optimization.get("runtimeCandidate", ""),
        "elapsedMs": float(user.get("elapsedMs", 0.0) or 0.0),
        "makespanMs": float(user.get("makespanMs", user.get("elapsedMs", 0.0)) or 0.0),
        "meanMs": float(user.get("meanMs", user.get("elapsedMs", 0.0)) or 0.0),
        "p50Ms": float(user.get("p50Ms", user.get("elapsedMs", 0.0)) or 0.0),
        "p95Ms": float(user.get("p95Ms", user.get("elapsedMs", 0.0)) or 0.0),
        "successCount": int(user.get("successCount", 1) or 0),
        "failureCount": int(user.get("failureCount", 0) or 0),
        "throughputRps": float(user.get("throughputRps", 0.0) or 0.0),
        "payloadBytes": int(user.get("payloadBytes", 0) or 0),
        "userExecution": user.get("status", ""),
        "dependencyExecution": summary.get("dependencyExecution", {}).get("status", ""),
        **provider_timing,
        **provider_capacity,
        **provider_allocation,
        **optimization_costs,
    }


def write_rows_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def parse_pad_list(raw: str) -> list[int]:
    values: list[int] = []
    for item in raw.split(","):
        text = item.strip()
        if not text:
            continue
        value = int(text)
        if value < 0:
            raise SystemExit("--activation-pad-bytes-list values must be non-negative")
        values.append(value)
    return values or [0]


def parse_delay_list(raw: str) -> list[float]:
    values: list[float] = []
    for item in raw.split(","):
        text = item.strip()
        if not text:
            continue
        value = float(text)
        if value < 0:
            raise SystemExit("--role-execution-delay-ms-list values must be non-negative")
        values.append(value)
    return values or [0.0]


def parse_assignment_list(raw: str) -> list[tuple[str, str]]:
    assignments: list[tuple[str, str]] = []
    for item in raw.split(","):
        assignment = item.strip()
        if not assignment:
            continue
        if assignment not in ASSIGNMENT_CANDIDATES:
            known = ", ".join(sorted(ASSIGNMENT_CANDIDATES))
            raise SystemExit(f"unknown assignment '{assignment}'; known: {known}")
        assignments.append((assignment, ASSIGNMENT_CANDIDATES[assignment]))
    if not assignments:
        raise SystemExit("--assignments must contain at least one assignment")
    return assignments


def delay_dir_name(value: float) -> str:
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    if not text:
        text = "0"
    return "delay-" + text.replace(".", "p")


def summarize_rows(rows: list[dict[str, Any]],
                   runs: int,
                   out_root: Path,
                   requests: int,
                   concurrency: int,
                   assignments: list[tuple[str, str]]) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "runsPerAssignment": runs,
        "requestsPerRun": requests,
        "concurrency": concurrency,
        "assignments": {},
        "rowsCsv": str(out_root / "campaign-runs.csv"),
    }
    for assignment, expected_candidate in assignments:
        assignment_rows = [row for row in rows if row["assignment"] == assignment]
        if not assignment_rows:
            continue
        elapsed = [float(row["elapsedMs"]) for row in assignment_rows]
        workload_mean = [float(row["meanMs"]) for row in assignment_rows]
        workload_p50 = [float(row["p50Ms"]) for row in assignment_rows]
        p95s = [float(row["p95Ms"]) for row in assignment_rows]
        throughput = [float(row["throughputRps"]) for row in assignment_rows]
        provider_queue_mean = [float(row["providerQueueWaitMeanMs"]) for row in assignment_rows]
        provider_queue_max = [float(row["providerQueueWaitMaxMs"]) for row in assignment_rows]
        provider_input_mean = [float(row["providerInputFetchWaitMeanMs"]) for row in assignment_rows]
        provider_input_max = [float(row["providerInputFetchWaitMaxMs"]) for row in assignment_rows]
        provider_runner_mean = [
            float(row["providerRunnerPublishMeanMs"]) for row in assignment_rows
        ]
        provider_runner_max = [
            float(row["providerRunnerPublishMaxMs"]) for row in assignment_rows
        ]
        capacity_rows = [float(row["providerCapacityRows"]) for row in assignment_rows]
        capacity_pending_mean = [
            float(row["providerCapacityPendingMean"]) for row in assignment_rows
        ]
        capacity_pending_max = [
            float(row["providerCapacityPendingMax"]) for row in assignment_rows
        ]
        capacity_ready_queue_mean = [
            float(row["providerCapacityReadyQueueMean"]) for row in assignment_rows
        ]
        capacity_ready_queue_max = [
            float(row["providerCapacityReadyQueueMax"]) for row in assignment_rows
        ]
        capacity_waiting_inputs_mean = [
            float(row["providerCapacityWaitingInputsMean"]) for row in assignment_rows
        ]
        capacity_waiting_inputs_max = [
            float(row["providerCapacityWaitingInputsMax"]) for row in assignment_rows
        ]
        capacity_active_workers_mean = [
            float(row["providerCapacityActiveWorkersMean"]) for row in assignment_rows
        ]
        capacity_active_workers_max = [
            float(row["providerCapacityActiveWorkersMax"]) for row in assignment_rows
        ]
        capacity_idle_workers_mean = [
            float(row["providerCapacityIdleWorkersMean"]) for row in assignment_rows
        ]
        capacity_worker_count_max = [
            float(row["providerCapacityWorkerCountMax"]) for row in assignment_rows
        ]
        provider_role_rows = [
            int(row["providerRoleExecutionRows"]) for row in assignment_rows
        ]
        backbone_default = [
            int(row["backboneDefaultExecutions"]) for row in assignment_rows
        ]
        backbone_replica = [
            int(row["backboneReplicaExecutions"]) for row in assignment_rows
        ]
        backbone_replica_share = [
            float(row["backboneReplicaShare"]) for row in assignment_rows
        ]
        selected_pressure = [
            float(row["selectedProviderReadyQueuePressureMs"]) for row in assignment_rows
        ]
        recommended_pressure = [
            float(row["recommendedProviderReadyQueuePressureMs"]) for row in assignment_rows
        ]
        shared_pressure = [
            float(row["sharedProviderReadyQueuePressureMs"]) for row in assignment_rows
        ]
        single_pressure = [
            float(row["singleProviderReadyQueuePressureMs"]) for row in assignment_rows
        ]
        selected_provider_utilization = [
            float(row["selectedProviderMaxUtilization"]) for row in assignment_rows
        ]
        selected_capacity_queue = [
            float(row["selectedProviderCapacityQueuePressureMs"]) for row in assignment_rows
        ]
        selected_dependency_rate = [
            float(row["selectedDependencyByteRateMbps"]) for row in assignment_rows
        ]
        selected_dependency_utilization = [
            float(row["selectedDependencyMaxLinkUtilization"]) for row in assignment_rows
        ]
        selected_dependency_pressure = [
            float(row["selectedDependencyRatePressureMs"]) for row in assignment_rows
        ]
        observed_candidates = sorted(set(str(row["selectedCandidate"]) for row in assignment_rows))
        summary["assignments"][assignment] = {
            "expectedCandidate": expected_candidate,
            "observedCandidates": observed_candidates,
            "successCount": sum(int(row["successCount"]) for row in assignment_rows),
            "failureCount": sum(int(row["failureCount"]) for row in assignment_rows),
            **stats(elapsed),
            "workloadMeanMeanMs": round(statistics.mean(workload_mean), 3) if workload_mean else 0.0,
            "workloadP50MeanMs": round(statistics.mean(workload_p50), 3) if workload_p50 else 0.0,
            "workloadP95MeanMs": round(statistics.mean(p95s), 3) if p95s else 0.0,
            "throughputMeanRps": round(statistics.mean(throughput), 3) if throughput else 0.0,
            "providerQueueWaitMeanMs": timing_mean(provider_queue_mean),
            "providerQueueWaitMaxMs": timing_max(provider_queue_max),
            "providerInputFetchWaitMeanMs": timing_mean(provider_input_mean),
            "providerInputFetchWaitMaxMs": timing_max(provider_input_max),
            "providerRunnerPublishMeanMs": timing_mean(provider_runner_mean),
            "providerRunnerPublishMaxMs": timing_max(provider_runner_max),
            "providerCapacityRows": int(sum(capacity_rows)),
            "providerCapacityPendingMean": timing_mean(capacity_pending_mean),
            "providerCapacityPendingMax": timing_max(capacity_pending_max),
            "providerCapacityReadyQueueMean": timing_mean(capacity_ready_queue_mean),
            "providerCapacityReadyQueueMax": timing_max(capacity_ready_queue_max),
            "providerCapacityWaitingInputsMean": timing_mean(capacity_waiting_inputs_mean),
            "providerCapacityWaitingInputsMax": timing_max(capacity_waiting_inputs_max),
            "providerCapacityActiveWorkersMean": timing_mean(capacity_active_workers_mean),
            "providerCapacityActiveWorkersMax": timing_max(capacity_active_workers_max),
            "providerCapacityIdleWorkersMean": timing_mean(capacity_idle_workers_mean),
            "providerCapacityWorkerCountMax": timing_max(capacity_worker_count_max),
            "providerRoleExecutionRows": int(sum(provider_role_rows)),
            "backboneDefaultExecutions": int(sum(backbone_default)),
            "backboneReplicaExecutions": int(sum(backbone_replica)),
            "backboneReplicaShareMean": timing_mean(backbone_replica_share),
            "selectedProviderReadyQueuePressureMs": timing_mean(selected_pressure),
            "recommendedProviderReadyQueuePressureMs": timing_mean(recommended_pressure),
            "sharedProviderReadyQueuePressureMs": timing_mean(shared_pressure),
            "singleProviderReadyQueuePressureMs": timing_mean(single_pressure),
            "selectedProviderMaxUtilization": timing_mean(selected_provider_utilization),
            "selectedProviderCapacityQueuePressureMs": timing_mean(selected_capacity_queue),
            "selectedDependencyByteRateMbps": timing_mean(selected_dependency_rate),
            "selectedDependencyMaxLinkUtilization": timing_mean(
                selected_dependency_utilization),
            "selectedDependencyRatePressureMs": timing_mean(selected_dependency_pressure),
        }

    baseline_name = assignments[0][0]
    alternative_name = assignments[1][0] if len(assignments) > 1 else ""
    comparison: dict[str, Any] = {
        "baselineAssignment": baseline_name,
        "alternativeAssignment": alternative_name,
    }
    if alternative_name:
        baseline = summary["assignments"][baseline_name]
        alternative = summary["assignments"][alternative_name]
        comparison.update({
        "meanDeltaMs": round(float(alternative["meanMs"]) - float(baseline["meanMs"]), 3),
        "p50DeltaMs": round(float(alternative["p50Ms"]) - float(baseline["p50Ms"]), 3),
        "p95DeltaMs": round(float(alternative["p95Ms"]) - float(baseline["p95Ms"]), 3),
        "meanRatio": (
            round(float(alternative["meanMs"]) / float(baseline["meanMs"]), 4)
            if float(baseline["meanMs"]) > 0 else None
        ),
        "workloadMeanDeltaMs": round(
            float(alternative["workloadMeanMeanMs"]) - float(baseline["workloadMeanMeanMs"]), 3),
        "workloadP50DeltaMs": round(
            float(alternative["workloadP50MeanMs"]) - float(baseline["workloadP50MeanMs"]), 3),
        "workloadP95DeltaMs": round(
            float(alternative["workloadP95MeanMs"]) - float(baseline["workloadP95MeanMs"]), 3),
        "providerQueueWaitMeanDeltaMs": round(
            float(alternative["providerQueueWaitMeanMs"]) -
            float(baseline["providerQueueWaitMeanMs"]), 3),
        "providerQueueWaitMaxDeltaMs": round(
            float(alternative["providerQueueWaitMaxMs"]) -
            float(baseline["providerQueueWaitMaxMs"]), 3),
        "backboneReplicaExecutionsDelta": int(
            alternative["backboneReplicaExecutions"]) -
            int(baseline["backboneReplicaExecutions"]),
        "backboneReplicaShareDelta": round(
            float(alternative["backboneReplicaShareMean"]) -
            float(baseline["backboneReplicaShareMean"]), 6),
        })
    summary["comparison"] = comparison
    return summary


def run_campaign(out_root: Path,
                 runs: int,
                 provider_check_timeout: int,
                 extra_args: list[str],
                 activation_pad_bytes: int,
                 role_execution_delay_ms: float,
                 requests: int,
                 concurrency: int,
                 target_rps: float = 0.0,
                 submission_spacing_ms: int = 0,
                 assignments: list[tuple[str, str]] | None = None) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    active_assignments = assignments or ASSIGNMENTS
    for assignment, _candidate in active_assignments:
        for run_index in range(1, runs + 1):
            rows.append(run_one(
                assignment,
                run_index,
                out_root,
                provider_check_timeout,
                extra_args,
                activation_pad_bytes,
                role_execution_delay_ms,
                requests,
                concurrency,
                target_rps,
                submission_spacing_ms))
    summary = summarize_rows(
        rows, runs, out_root, requests, concurrency, active_assignments)
    summary["activationPadBytes"] = activation_pad_bytes
    summary["roleExecutionDelayMs"] = role_execution_delay_ms
    summary["targetRps"] = target_rps
    summary["submissionSpacingMs"] = submission_spacing_ms
    write_rows_csv(out_root / "campaign-runs.csv", rows)
    (out_root / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--out-root", required=True)
    parser.add_argument("--provider-check-timeout", type=int, default=60)
    parser.add_argument("--core-trace", action="store_true")
    parser.add_argument("--activation-pad-bytes-list", default="0",
                        help="Comma-separated Backbone activation padding sizes")
    parser.add_argument("--role-execution-delay-ms-list", default="0",
                        help="Comma-separated per-role execution delay values")
    parser.add_argument("--requests", type=int, default=1,
                        help="Closed-loop NativeTracer requests per run")
    parser.add_argument("--concurrency", type=int, default=1,
                        help="Maximum outstanding NativeTracer requests per run")
    parser.add_argument("--target-rps", type=float, default=0.0,
                        help="Optional target request rate for planner cost evidence")
    parser.add_argument("--submission-spacing-ms", type=int, default=0,
                        help="Optional delay between concurrent user submissions")
    parser.add_argument("--assignments", default="default,single-provider",
                        help=("Comma-separated assignment list. Known values: "
                              "auto, default, single-provider, capacity-pool"))
    args = parser.parse_args(argv)
    if args.runs <= 0:
        raise SystemExit("--runs must be positive")
    if args.requests <= 0:
        raise SystemExit("--requests must be positive")
    if args.concurrency <= 0:
        raise SystemExit("--concurrency must be positive")
    if args.concurrency > args.requests:
        args.concurrency = args.requests
    if args.target_rps < 0.0:
        raise SystemExit("--target-rps must be non-negative")
    if args.submission_spacing_ms < 0:
        raise SystemExit("--submission-spacing-ms must be non-negative")

    out_root = Path(args.out_root).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    extra_args = ["--core-trace"] if args.core_trace else []
    pad_values = parse_pad_list(args.activation_pad_bytes_list)
    delay_values = parse_delay_list(args.role_execution_delay_ms_list)
    assignments = parse_assignment_list(args.assignments)

    if len(pad_values) == 1 and len(delay_values) == 1:
        summary = run_campaign(
            out_root,
            args.runs,
            args.provider_check_timeout,
            extra_args,
            pad_values[0],
            delay_values[0],
            args.requests,
            args.concurrency,
            args.target_rps,
            args.submission_spacing_ms,
            assignments)
        print("NDNSF_DI_NATIVE_TRACER_LAYOUT_CAMPAIGN_OK")
        print(json.dumps(summary["assignments"], indent=2, sort_keys=True))
        print(json.dumps(summary["comparison"], indent=2, sort_keys=True))
        return 0

    if len(delay_values) > 1:
        capacity: dict[str, Any] = {
            "runsPerAssignment": args.runs,
            "requestsPerRun": args.requests,
            "concurrency": args.concurrency,
            "padValues": pad_values,
            "delayValuesMs": delay_values,
            "campaigns": {},
        }
        for delay_ms in delay_values:
            delay_key = f"{delay_ms:.3f}".rstrip("0").rstrip(".") or "0"
            capacity["campaigns"][delay_key] = {}
            delay_root = out_root / delay_dir_name(delay_ms)
            delay_root.mkdir(parents=True, exist_ok=True)
            for pad_bytes in pad_values:
                pad_root = delay_root / f"pad-{pad_bytes}"
                pad_root.mkdir(parents=True, exist_ok=True)
                capacity["campaigns"][delay_key][str(pad_bytes)] = run_campaign(
                    pad_root,
                    args.runs,
                    args.provider_check_timeout,
                    extra_args,
                    pad_bytes,
                    delay_ms,
                    args.requests,
                    args.concurrency,
                    args.target_rps,
                    args.submission_spacing_ms,
                    assignments)
        (out_root / "capacity-summary.json").write_text(
            json.dumps(capacity, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        print("NDNSF_DI_NATIVE_TRACER_CAPACITY_CAMPAIGN_OK")
        print(json.dumps(capacity["campaigns"], indent=2, sort_keys=True))
        return 0

    threshold: dict[str, Any] = {
        "runsPerAssignment": args.runs,
        "requestsPerRun": args.requests,
        "concurrency": args.concurrency,
        "padValues": pad_values,
        "roleExecutionDelayMs": delay_values[0],
        "campaigns": {},
    }
    for pad_bytes in pad_values:
        pad_root = out_root / f"pad-{pad_bytes}"
        pad_root.mkdir(parents=True, exist_ok=True)
        threshold["campaigns"][str(pad_bytes)] = run_campaign(
            pad_root,
            args.runs,
            args.provider_check_timeout,
            extra_args,
            pad_bytes,
            delay_values[0],
            args.requests,
            args.concurrency,
            args.target_rps,
            args.submission_spacing_ms,
            assignments)
    (out_root / "threshold-summary.json").write_text(
        json.dumps(threshold, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print("NDNSF_DI_NATIVE_TRACER_THRESHOLD_CAMPAIGN_OK")
    print(json.dumps(threshold["campaigns"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
