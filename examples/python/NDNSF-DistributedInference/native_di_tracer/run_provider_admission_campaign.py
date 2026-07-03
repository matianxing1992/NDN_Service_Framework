#!/usr/bin/env python3
"""Compare NativeTracer MiniNDN runs with and without provider admission policy."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[4]
HARNESS = REPO / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def run_case(args: argparse.Namespace,
             name: str,
             extra_args: list[str]) -> dict[str, Any]:
    out_dir = args.out / name
    command = [
        "sudo", "-n", "python3", str(HARNESS),
        "--full-network",
        "--tracer-deterministic-runner",
        "--requests", str(args.requests),
        "--concurrency", str(args.concurrency),
        "--submission-spacing-ms", str(args.submission_spacing_ms),
        "--role-execution-delay-ms", str(args.role_execution_delay_ms),
        "--out", str(out_dir),
    ] + extra_args
    completed = subprocess.run(command, cwd=str(REPO), check=False)
    summary_path = out_dir / "summary.json"
    if summary_path.exists():
        summary = read_json(summary_path)
    else:
        summary = {
            "status": "MISSING_SUMMARY",
            "failureReason": f"harness exited {completed.returncode} without summary",
        }
    user = summary.get("userExecution", {})
    return {
        "name": name,
        "returncode": completed.returncode,
        "resultDir": str(out_dir),
        "status": summary.get("status", ""),
        "failureReason": summary.get("failureReason", ""),
        "providerAdmissionPolicy": summary.get("providerAdmissionPolicy", {}),
        "requestCount": user.get("requestCount", args.requests),
        "successCount": user.get("successCount", 0),
        "failureCount": user.get("failureCount", 0),
        "p50Ms": user.get("p50Ms", 0.0),
        "p95Ms": user.get("p95Ms", 0.0),
        "meanMs": user.get("meanMs", 0.0),
        "throughputRps": user.get("throughputRps", 0.0),
        "failureBreakdown": summary.get("failureBreakdown", {}),
        "negativeAckReasonCounters": summary.get("negativeAckReasonCounters", {}),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=(
        REPO / "results/native_di_real_minindn/provider_admission_campaign"))
    parser.add_argument("--requests", type=int, default=3)
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--submission-spacing-ms", type=int, default=300)
    parser.add_argument("--role-execution-delay-ms", type=float, default=750.0)
    parser.add_argument("--max-active-workers", type=int, default=1)
    parser.add_argument("--max-queue", type=int, default=-1)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    cases = [
        run_case(args, "baseline", []),
        run_case(args, "provider-admission", [
            "--provider-admission-max-active-workers", str(args.max_active_workers),
            "--provider-admission-max-queue", str(args.max_queue),
        ]),
    ]
    aggregate = {
        "campaign": "provider-admission",
        "requests": args.requests,
        "concurrency": args.concurrency,
        "submissionSpacingMs": args.submission_spacing_ms,
        "roleExecutionDelayMs": args.role_execution_delay_ms,
        "cases": cases,
    }
    summary_path = args.out / "provider-admission-campaign-summary.json"
    summary_path.write_text(json.dumps(aggregate, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    print(summary_path)
    print(json.dumps(aggregate, indent=2, sort_keys=True))
    return 0 if all(item["status"] in {"SUCCESS", "FAILURE"} for item in cases) else 1


if __name__ == "__main__":
    sys.exit(main())
