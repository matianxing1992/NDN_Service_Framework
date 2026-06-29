#!/usr/bin/env python3
"""Run repeated MiniNDN NativeTracer auto-assignment measurements."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import statistics
import sys
from typing import Any

from run_layout_campaign import run_one, stats


DEFAULT_WORKLOADS = "c1:1:1,c2:2:2,c4:4:4"


def parse_workloads(raw: str) -> list[tuple[str, int, int]]:
    workloads: list[tuple[str, int, int]] = []
    for item in raw.split(","):
        text = item.strip()
        if not text:
            continue
        parts = text.split(":")
        if len(parts) == 1:
            concurrency = int(parts[0])
            label = f"c{concurrency}"
            requests = concurrency
        elif len(parts) == 2:
            label = parts[0]
            concurrency = int(parts[1])
            requests = concurrency
        elif len(parts) == 3:
            label = parts[0]
            requests = int(parts[1])
            concurrency = int(parts[2])
        else:
            raise SystemExit(
                "--workloads entries must be concurrency, label:concurrency, "
                "or label:requests:concurrency")
        if requests <= 0 or concurrency <= 0:
            raise SystemExit("--workloads requests and concurrency must be positive")
        if concurrency > requests:
            raise SystemExit("--workloads concurrency cannot exceed requests")
        workloads.append((label, requests, concurrency))
    if not workloads:
        raise SystemExit("--workloads must contain at least one workload")
    return workloads


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        raise RuntimeError("cannot write empty campaign CSV")
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def summarize_workload(rows: list[dict[str, Any]],
                       label: str,
                       requests: int,
                       concurrency: int) -> dict[str, Any]:
    elapsed = [float(row["elapsedMs"]) for row in rows]
    workload_mean = [float(row["meanMs"]) for row in rows]
    workload_p50 = [float(row["p50Ms"]) for row in rows]
    workload_p95 = [float(row["p95Ms"]) for row in rows]
    throughput = [float(row["throughputRps"]) for row in rows]
    provider_queue = [float(row["providerQueueWaitMeanMs"]) for row in rows]
    provider_queue_max = [float(row["providerQueueWaitMaxMs"]) for row in rows]
    capacity_rows = [float(row["providerCapacityRows"]) for row in rows]
    capacity_pending_mean = [float(row["providerCapacityPendingMean"]) for row in rows]
    capacity_pending_max = [float(row["providerCapacityPendingMax"]) for row in rows]
    capacity_ready_queue_mean = [
        float(row["providerCapacityReadyQueueMean"]) for row in rows
    ]
    capacity_ready_queue_max = [
        float(row["providerCapacityReadyQueueMax"]) for row in rows
    ]
    capacity_waiting_inputs_mean = [
        float(row["providerCapacityWaitingInputsMean"]) for row in rows
    ]
    capacity_waiting_inputs_max = [
        float(row["providerCapacityWaitingInputsMax"]) for row in rows
    ]
    capacity_active_workers_mean = [
        float(row["providerCapacityActiveWorkersMean"]) for row in rows
    ]
    capacity_active_workers_max = [
        float(row["providerCapacityActiveWorkersMax"]) for row in rows
    ]
    selected_pressure = [
        float(row["selectedProviderReadyQueuePressureMs"]) for row in rows
    ]
    recommended_pressure = [
        float(row["recommendedProviderReadyQueuePressureMs"]) for row in rows
    ]
    shared_pressure = [float(row["sharedProviderReadyQueuePressureMs"]) for row in rows]
    single_pressure = [float(row["singleProviderReadyQueuePressureMs"]) for row in rows]
    selected_provider_utilization = [
        float(row["selectedProviderMaxUtilization"]) for row in rows
    ]
    selected_capacity_queue = [
        float(row["selectedProviderCapacityQueuePressureMs"]) for row in rows
    ]
    selected_dependency_rate = [
        float(row["selectedDependencyByteRateMbps"]) for row in rows
    ]
    selected_dependency_utilization = [
        float(row["selectedDependencyMaxLinkUtilization"]) for row in rows
    ]
    selected_dependency_pressure = [
        float(row["selectedDependencyRatePressureMs"]) for row in rows
    ]
    return {
        "label": label,
        "requestsPerRun": requests,
        "concurrency": concurrency,
        "assignmentRequested": "auto",
        "resolvedAssignments": sorted(set(str(row["assignmentResolved"]) for row in rows)),
        "selectedCandidates": sorted(set(str(row["selectedCandidate"]) for row in rows)),
        "successCount": sum(int(row["successCount"]) for row in rows),
        "failureCount": sum(int(row["failureCount"]) for row in rows),
        **stats(elapsed),
        "workloadMeanMeanMs": round(statistics.mean(workload_mean), 3),
        "workloadP50MeanMs": round(statistics.mean(workload_p50), 3),
        "workloadP95MeanMs": round(statistics.mean(workload_p95), 3),
        "throughputMeanRps": round(statistics.mean(throughput), 3),
        "providerQueueWaitMeanMs": round(statistics.mean(provider_queue), 3),
        "providerQueueWaitMaxMs": round(max(provider_queue_max), 3),
        "providerCapacityRows": int(sum(capacity_rows)),
        "providerCapacityPendingMean": round(statistics.mean(capacity_pending_mean), 3),
        "providerCapacityPendingMax": round(max(capacity_pending_max), 3),
        "providerCapacityReadyQueueMean": round(
            statistics.mean(capacity_ready_queue_mean), 3),
        "providerCapacityReadyQueueMax": round(max(capacity_ready_queue_max), 3),
        "providerCapacityWaitingInputsMean": round(
            statistics.mean(capacity_waiting_inputs_mean), 3),
        "providerCapacityWaitingInputsMax": round(max(capacity_waiting_inputs_max), 3),
        "providerCapacityActiveWorkersMean": round(
            statistics.mean(capacity_active_workers_mean), 3),
        "providerCapacityActiveWorkersMax": round(max(capacity_active_workers_max), 3),
        "selectedProviderReadyQueuePressureMs": round(statistics.mean(selected_pressure), 3),
        "recommendedProviderReadyQueuePressureMs": round(statistics.mean(recommended_pressure), 3),
        "sharedProviderReadyQueuePressureMs": round(statistics.mean(shared_pressure), 3),
        "singleProviderReadyQueuePressureMs": round(statistics.mean(single_pressure), 3),
        "selectedProviderMaxUtilization": round(
            statistics.mean(selected_provider_utilization), 6),
        "selectedProviderCapacityQueuePressureMs": round(
            statistics.mean(selected_capacity_queue), 3),
        "selectedDependencyByteRateMbps": round(statistics.mean(selected_dependency_rate), 6),
        "selectedDependencyMaxLinkUtilization": round(
            statistics.mean(selected_dependency_utilization), 6),
        "selectedDependencyRatePressureMs": round(
            statistics.mean(selected_dependency_pressure), 3),
    }


def expected_candidate(concurrency: int) -> str:
    if concurrency == 1:
        return "single-provider-serial"
    return "shared-backbone-current"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--out-root", required=True)
    parser.add_argument("--provider-check-timeout", type=int, default=60)
    parser.add_argument("--core-trace", action="store_true")
    parser.add_argument("--activation-pad-bytes", type=int, default=0)
    parser.add_argument("--role-execution-delay-ms", type=float, default=75.0)
    parser.add_argument("--target-rps", type=float, default=0.0,
                        help="Optional target request rate for planner cost evidence")
    parser.add_argument("--workloads", default=DEFAULT_WORKLOADS,
                        help=("Comma-separated workload list. Entries may be "
                              "concurrency, label:concurrency, or "
                              "label:requests:concurrency."))
    args = parser.parse_args(argv)

    if args.runs <= 0:
        raise SystemExit("--runs must be positive")
    if args.activation_pad_bytes < 0:
        raise SystemExit("--activation-pad-bytes must be non-negative")
    if args.role_execution_delay_ms < 0:
        raise SystemExit("--role-execution-delay-ms must be non-negative")
    if args.target_rps < 0.0:
        raise SystemExit("--target-rps must be non-negative")

    out_root = Path(args.out_root).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    extra_args = ["--core-trace"] if args.core_trace else []
    workloads = parse_workloads(args.workloads)

    rows: list[dict[str, Any]] = []
    workload_summaries: dict[str, Any] = {}
    for label, requests, concurrency in workloads:
        workload_root = out_root / label
        workload_root.mkdir(parents=True, exist_ok=True)
        workload_rows: list[dict[str, Any]] = []
        for run_index in range(1, args.runs + 1):
            row = run_one(
                "auto",
                run_index,
                workload_root,
                args.provider_check_timeout,
                extra_args,
                args.activation_pad_bytes,
                args.role_execution_delay_ms,
                requests,
                concurrency,
                args.target_rps)
            row = {
                "workload": label,
                "expectedCandidate": expected_candidate(concurrency),
                **row,
            }
            workload_rows.append(row)
            rows.append(row)
        workload_summaries[label] = summarize_workload(
            workload_rows,
            label,
            requests,
            concurrency)
        workload_summaries[label]["expectedCandidate"] = expected_candidate(concurrency)

    rows_csv = out_root / "auto-campaign-runs.csv"
    summary = {
        "runsPerWorkload": args.runs,
        "activationPadBytes": args.activation_pad_bytes,
        "roleExecutionDelayMs": args.role_execution_delay_ms,
        "targetRps": args.target_rps,
        "workloads": workload_summaries,
        "rowsCsv": str(rows_csv),
    }
    write_csv(rows_csv, rows)
    (out_root / "auto-campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")

    print("NDNSF_DI_NATIVE_TRACER_AUTO_ASSIGNMENT_CAMPAIGN_OK")
    print(json.dumps(summary["workloads"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
