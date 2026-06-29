#!/usr/bin/env python3
"""Sweep NativeTracer target RPS planner evidence and optional MiniNDN auto runs."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys
from typing import Any

from run_layout_campaign import run_one


ROOT = Path(__file__).resolve().parent
PLAN_TRACER = ROOT / "plan_tracer.py"
SHARED = "shared-backbone-current"
SINGLE = "single-provider-serial"


def parse_float_list(raw: str) -> list[float]:
    values: list[float] = []
    for item in raw.split(","):
        text = item.strip()
        if not text:
            continue
        value = float(text)
        if value < 0.0:
            raise SystemExit("RPS values must be non-negative")
        values.append(value)
    return values


def rps_dir_name(value: float) -> str:
    text = f"{value:.3f}".rstrip("0").rstrip(".") or "0"
    return "rps-" + text.replace(".", "p")


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def candidate_cost(evidence: dict[str, Any], candidate_id: str) -> dict[str, Any]:
    for candidate in evidence.get("candidates", []):
        if candidate.get("id") == candidate_id:
            return dict(candidate.get("cost", {}) or {})
    raise RuntimeError(f"candidate not found: {candidate_id}")


def run_planner(out_root: Path,
                target_rps: float,
                role_execution_delay_ms: float,
                activation_pad_bytes: int,
                concurrency: int) -> dict[str, Any]:
    run_dir = out_root / "planner" / rps_dir_name(target_rps)
    run_dir.mkdir(parents=True, exist_ok=True)
    summary_path = run_dir / "summary.json"
    subprocess.run(
        [
            "python3", str(PLAN_TRACER),
            "--out", str(run_dir),
            "--summary-json", str(summary_path),
            "--runtime-candidate", SHARED,
            "--activation-pad-bytes", str(activation_pad_bytes),
            "--role-execution-delay-ms", str(role_execution_delay_ms),
            "--workload-concurrency", str(concurrency),
            "--target-rps", str(target_rps),
        ],
        cwd=str(ROOT),
        check=True,
    )
    evidence_path = run_dir / "planner-optimization.json"
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    shared = candidate_cost(evidence, SHARED)
    single = candidate_cost(evidence, SINGLE)
    recommended = str(evidence.get("plannerRecommendedCandidate", {}).get("id", ""))
    return {
        "targetRps": target_rps,
        "recommendedCandidate": recommended,
        "sharedTotalEstimatedMs": shared["totalEstimatedMs"],
        "singleTotalEstimatedMs": single["totalEstimatedMs"],
        "sharedProviderMaxUtilization": shared["providerMaxUtilization"],
        "singleProviderMaxUtilization": single["providerMaxUtilization"],
        "sharedProviderCapacityQueuePressureMs": shared[
            "providerCapacityQueuePressureMs"],
        "singleProviderCapacityQueuePressureMs": single[
            "providerCapacityQueuePressureMs"],
        "sharedDependencyByteRateMbps": shared["dependencyByteRateMbps"],
        "singleDependencyByteRateMbps": single["dependencyByteRateMbps"],
        "sharedDependencyMaxLinkUtilization": shared["dependencyMaxLinkUtilization"],
        "singleDependencyMaxLinkUtilization": single["dependencyMaxLinkUtilization"],
        "sharedDependencyRatePressureMs": shared["dependencyRatePressureMs"],
        "singleDependencyRatePressureMs": single["dependencyRatePressureMs"],
        "sharedProviderReadyQueuePressureMs": shared["providerReadyQueuePressureMs"],
        "singleProviderReadyQueuePressureMs": single["providerReadyQueuePressureMs"],
        "evidence": str(evidence_path),
    }


def run_minindn_auto(out_root: Path,
                     target_rps: float,
                     provider_check_timeout: int,
                     activation_pad_bytes: int,
                     role_execution_delay_ms: float,
                     requests: int,
                     concurrency: int) -> dict[str, Any]:
    row = run_one(
        "auto",
        1,
        out_root / "minindn" / rps_dir_name(target_rps),
        provider_check_timeout,
        [],
        activation_pad_bytes,
        role_execution_delay_ms,
        requests,
        concurrency,
        target_rps,
    )
    return {
        "targetRps": target_rps,
        "assignmentResolved": row["assignmentResolved"],
        "selectedCandidate": row["selectedCandidate"],
        "successCount": row["successCount"],
        "failureCount": row["failureCount"],
        "workloadMeanMs": round(float(row["meanMs"]), 3),
        "workloadP95Ms": round(float(row["p95Ms"]), 3),
        "throughputRps": round(float(row["throughputRps"]), 3),
        "providerQueueWaitMeanMs": round(float(row["providerQueueWaitMeanMs"]), 3),
        "providerQueueWaitMaxMs": round(float(row["providerQueueWaitMaxMs"]), 3),
        "providerCapacityRows": int(row["providerCapacityRows"]),
        "providerCapacityPendingMean": round(
            float(row["providerCapacityPendingMean"]), 3),
        "providerCapacityPendingMax": round(
            float(row["providerCapacityPendingMax"]), 3),
        "providerCapacityReadyQueueMean": round(
            float(row["providerCapacityReadyQueueMean"]), 3),
        "providerCapacityReadyQueueMax": round(
            float(row["providerCapacityReadyQueueMax"]), 3),
        "providerCapacityWaitingInputsMean": round(
            float(row["providerCapacityWaitingInputsMean"]), 3),
        "providerCapacityWaitingInputsMax": round(
            float(row["providerCapacityWaitingInputsMax"]), 3),
        "providerCapacityActiveWorkersMean": round(
            float(row["providerCapacityActiveWorkersMean"]), 3),
        "providerCapacityActiveWorkersMax": round(
            float(row["providerCapacityActiveWorkersMax"]), 3),
        "selectedProviderMaxUtilization": round(
            float(row["selectedProviderMaxUtilization"]), 6),
        "selectedProviderCapacityQueuePressureMs": round(
            float(row["selectedProviderCapacityQueuePressureMs"]), 3),
        "selectedDependencyByteRateMbps": round(
            float(row["selectedDependencyByteRateMbps"]), 6),
        "selectedDependencyRatePressureMs": round(
            float(row["selectedDependencyRatePressureMs"]), 3),
        "resultDir": row["resultDir"],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-root", required=True)
    parser.add_argument("--target-rps-list", default="0,1,2,4,8")
    parser.add_argument("--minindn-rps-list", default="",
                        help="Optional subset of RPS values to run through MiniNDN auto")
    parser.add_argument("--provider-check-timeout", type=int, default=60)
    parser.add_argument("--activation-pad-bytes", type=int, default=0)
    parser.add_argument("--role-execution-delay-ms", type=float, default=75.0)
    parser.add_argument("--requests", type=int, default=4)
    parser.add_argument("--concurrency", type=int, default=4)
    args = parser.parse_args(argv)

    if args.activation_pad_bytes < 0:
        raise SystemExit("--activation-pad-bytes must be non-negative")
    if args.role_execution_delay_ms < 0.0:
        raise SystemExit("--role-execution-delay-ms must be non-negative")
    if args.requests <= 0 or args.concurrency <= 0:
        raise SystemExit("--requests and --concurrency must be positive")
    if args.concurrency > args.requests:
        args.concurrency = args.requests

    out_root = Path(args.out_root).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    planner_rps = parse_float_list(args.target_rps_list)
    minindn_rps = parse_float_list(args.minindn_rps_list)

    planner_rows = [
        run_planner(
            out_root,
            target_rps,
            args.role_execution_delay_ms,
            args.activation_pad_bytes,
            args.concurrency,
        )
        for target_rps in planner_rps
    ]
    minindn_rows = [
        run_minindn_auto(
            out_root,
            target_rps,
            args.provider_check_timeout,
            args.activation_pad_bytes,
            args.role_execution_delay_ms,
            args.requests,
            args.concurrency,
        )
        for target_rps in minindn_rps
    ]

    planner_csv = out_root / "rate-planner-sweep.csv"
    minindn_csv = out_root / "rate-minindn-sweep.csv"
    write_csv(planner_csv, planner_rows)
    write_csv(minindn_csv, minindn_rows)
    summary = {
        "targetRpsList": planner_rps,
        "minindnRpsList": minindn_rps,
        "requests": args.requests,
        "concurrency": args.concurrency,
        "activationPadBytes": args.activation_pad_bytes,
        "roleExecutionDelayMs": args.role_execution_delay_ms,
        "plannerCsv": str(planner_csv),
        "minindnCsv": str(minindn_csv) if minindn_rows else "",
        "planner": planner_rows,
        "minindn": minindn_rows,
    }
    (out_root / "rate-sweep-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print("NDNSF_DI_NATIVE_TRACER_RATE_SWEEP_OK")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
