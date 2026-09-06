#!/usr/bin/env python3
"""Spec176 MiniNDN failure matrix definition and runner.

The matrix is intentionally data-driven: every case states the expected
terminal state and safety invariant.  It can be executed after the nominal
launcher has been proven on the target host; locally it always emits the
frozen case manifest before attempting any network work.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NOMINAL = Path(__file__).with_name("minindn_uav_collaboration.py")

CASES = [
    ("no-feasible-detector", "FAILED", "no Ground Station fallback unless explicitly enabled"),
    ("compute-uav-loss", "FAILED", "mission remains active and no command is replayed"),
    ("fallback-disabled", "FAILED", "no implicit fallback"),
    ("fallback-enabled", "SUCCEEDED", "fallback is explicit and marked degraded"),
    ("evidence-expiry", "FAILED", "expired named Data is rejected"),
    ("evidence-tamper", "FAILED", "wrong signature/name/digest is rejected"),
    ("selected-provider-loss", "FAILED", "terminal owner does not change silently"),
    ("delayed-older-result", "FAILED", "prior attempt cannot close the new job"),
    ("terminal-delivery-loss", "FAILED", "named report may recover; duplicate terminal is rejected"),
    ("bounded-queue-overload", "FAILED", "command lifecycle remains responsive"),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--window-seconds", type=int, default=8,
                        help="bounded per-case measured window (default: 8)")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "results/spec176-uav-failure-matrix")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.window_seconds <= 0:
        parser.error("--window-seconds must be positive")
    manifest = {
        "schemaVersion": "spec176-uav-minindn-failure-matrix-v1",
        "cases": [
            {"name": name, "expectedTerminalState": state, "invariant": invariant}
            for name, state, invariant in CASES
        ],
        "fixedTopology": ["gs", "scout-a", "scout-b", "compute"],
        "fixedContract": "named producer Data; no IP/host/port application fields",
        "perCaseWindowSeconds": args.window_seconds,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if not args.run:
        print(json.dumps(manifest, indent=2))
        return 0
    results = []
    for name, expected_state, invariant in CASES:
        case_output = args.output / name
        case_output.mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(NOMINAL), "--run",
                   "--failure-case", name,
                   "--window-seconds", str(args.window_seconds),
                   # A fresh isolated MiniNDN process can spend several
                   # hundred milliseconds creating identities and fetching
                   # permissions.  Keep the application deadline finite, but
                   # do not classify that cold-start work as a provider
                   # failure before the evidence-source ACK can arrive.
                   "--ack-timeout-ms", "1500",
                   "--timeout-ms", "5000",
                   "--output", str(case_output)]
        if os.geteuid() != 0:
            unshare = shutil.which("unshare")
            if unshare is None:
                raise RuntimeError("MiniNDN failure matrix requires root or unshare")
            command = [unshare, "--user", "--map-root-user", "--mount", "--net", *command]
        started = time.monotonic()
        result = subprocess.run(command, check=False, env=os.environ.copy(),
                                timeout=max(90, args.window_seconds + 75))
        summary_path = case_output / "summary.json"
        logs = list(case_output.glob("*.log"))
        text = "\n".join(path.read_text(encoding="utf-8", errors="replace")
                           for path in logs)
        expected_success = expected_state == "SUCCEEDED"
        observed_success = "GS_INCIDENT_COLLABORATION_EXIT ok=true" in text
        observed_failure = "GS_INCIDENT_COLLABORATION_EXIT ok=false" in text
        case_ok = observed_success if expected_success else observed_failure
        if name == "fallback-enabled":
            case_ok = case_ok and "GS_FALLBACK_EXECUTED" in text
        results.append({
            "name": name,
            "expectedTerminalState": expected_state,
            "invariant": invariant,
            "returnCode": result.returncode,
            "elapsedSeconds": round(time.monotonic() - started, 3),
            "observedSuccess": observed_success,
            "observedFailure": observed_failure,
            "caseStatus": "PASS" if case_ok else "FAIL",
            "output": str(case_output),
            "summaryPresent": summary_path.exists(),
        })
    summary = {
        "schemaVersion": "spec176-uav-minindn-failure-summary-v1",
        "cases": results,
        "status": "PASS" if all(item["caseStatus"] == "PASS" for item in results)
                  else "FAIL",
    }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
