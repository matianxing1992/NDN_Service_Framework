#!/usr/bin/env python3
"""Single-writer, exact-once Spec 129 12-cell confirmation runner."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "Experiments/NDNSF_DI_SelectionGatedDeployment_Minindn.py"
FORMAL_FREEZE_MARKER = ROOT / "specs/129-selection-gated-deployment/FROZEN.md"
SPEC128_SPEC = ROOT / "specs/128-generic-multiloss-recovery"
SPEC128_RESULTS = tuple(sorted((ROOT / "results").glob("spec128-*")))
FROZEN_SOURCES = tuple(ROOT / value for value in (
    "ndn-service-framework/NDNSFMessages.hpp",
    "ndn-service-framework/NDNSFMessages.cpp",
    "ndn-service-framework/ServiceUser.hpp",
    "ndn-service-framework/ServiceUser.cpp",
    "ndn-service-framework/ServiceProvider.hpp",
    "ndn-service-framework/ServiceProvider.cpp",
    "ndn-service-framework/HybridMessageCrypto.hpp",
    "ndn-service-framework/HybridMessageCrypto.cpp",
    "NDNSF-DistributedInference/ndnsf_distributed_inference/core/deployment_control.py",
    "NDNSF-DistributedInference/ndnsf_distributed_inference/core/secure_status.py",
    "Experiments/NDNSF_DI_SelectionGatedDeployment_Minindn.py",
    "Experiments/run_spec129_selection_gated_deployment_matrix.py",
))

METRIC_SCHEMA = (
    "requestCount", "ackCount", "positiveAckCount", "negativeAckCount",
    "reservationCreatedCount", "reservationCommittedCount",
    "decisionSelectedCount", "decisionNotSelectedCount", "decisionReceiptCount",
    "releaseCount", "releaseReceiptCount", "leaseExpiryFallbackCount",
    "retryCount", "retryExhaustionCount", "timeoutCount", "nackCount",
    "requestBytes", "ackBytes", "selectionBytes", "responseBytes",
    "stageInputCount", "stageAbortCount", "statusQueryCount",
    "statusSnapshotCount", "tamperRejectCount", "replayRejectCount",
    "staleRejectCount",
    "preSelectionMutationCount", "selectedPreparationCount",
    "unselectedPreparationCount", "executionCount", "duplicateExecutionCount",
    "cleanupCount", "overlapMs", "completionLatencyMs",
    "p50LatencyMs", "p95LatencyMs", "tailLatencyMs",
    "packetPlaintextMatches",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def frozen_cells() -> list[dict[str, Any]]:
    scenarios = (
        (1, "baseline-three-provider", "baseline"),
        (2, "input-only", "independent-capability"),
        (3, "lost-negative-decision", "decision-loss"),
        (4, "lost-decision-receipt", "receipt-loss"),
        (5, "stale-conflicting-decision", "stale-conflict"),
        (6, "input-tamper", "ciphertext-tamper"),
        (7, "provider-restart", "provider-restart"),
        (8, "partial-reservation-contention", "partial-contention"),
        (9, "dependency-branch-overlap", "dependency-overlap"),
        (10, "contention-retry-exhaustion", "retry-exhaustion"),
        (11, "authorized-status-cursor", "status-authorized"),
        (12, "adversarial-status", "status-adversarial"),
    )
    return [{
        "cell": number, "id": name, "faultProfile": fault,
        "automaticRetry": False, "rerunAllowed": False,
        "maximumDecisionRetries": 2, "maximumContentionAttempts": 2,
        "maximumStatusQueries": 8,
    } for number, name, fault in scenarios]


def digest_path(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except PermissionError:
        completed = subprocess.run(
            ["sudo", "-n", "sha256sum", "--", str(path)], text=True,
            capture_output=True, check=True)
        return completed.stdout.split()[0]


def hash_files(paths: Iterable[Path]) -> dict[str, str]:
    output = {}
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"missing frozen source: {path}")
        output[str(path.relative_to(ROOT))] = digest_path(path)
    return output


def hash_tree(path: Path) -> dict[str, str]:
    if not path.is_dir():
        raise RuntimeError(f"missing immutable tree: {path}")
    return {str(item.relative_to(ROOT)): digest_path(item)
            for item in sorted(path.rglob("*")) if item.is_file()}


def hash_spec128_evidence() -> dict[str, str]:
    hashes = hash_tree(SPEC128_SPEC)
    if not SPEC128_RESULTS:
        raise RuntimeError("missing immutable Spec 128 result evidence")
    for result in SPEC128_RESULTS:
        hashes.update(hash_tree(result))
    return hashes


def validate_manifest(cells: list[dict[str, Any]]) -> None:
    if len(cells) != 12 or [cell["cell"] for cell in cells] != list(range(1, 13)):
        raise RuntimeError("Spec 129 manifest must contain ordered cells 1..12")
    if len({cell["id"] for cell in cells}) != 12:
        raise RuntimeError("Spec 129 manifest contains duplicate cell IDs")
    if any(cell["automaticRetry"] or cell["rerunAllowed"] for cell in cells):
        raise RuntimeError("formal Spec 129 cells cannot retry or rerun automatically")


def owner_processes(process_table: str, ignored_pids: set[int] | None = None) -> list[str]:
    ignored = set(ignored_pids or ()) | {os.getpid()}
    tokens = (LAUNCHER.name, Path(__file__).name, "minindn")
    owners = []
    for line in process_table.splitlines():
        fields = line.split(None, 1)
        if fields and fields[0].isdigit() and int(fields[0]) in ignored:
            continue
        if any(token in line for token in tokens):
            owners.append(line)
    return owners


def process_ancestry(pid: int | None = None) -> set[int]:
    current = os.getpid() if pid is None else int(pid)
    result = set()
    while current > 1 and current not in result:
        result.add(current)
        try:
            current = int(Path(f"/proc/{current}/stat").read_text(
                encoding="utf-8").split()[3])
        except (FileNotFoundError, IndexError, ValueError):
            break
    return result


def build_identity() -> dict[str, Any]:
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
        capture_output=True, check=False).stdout.strip()
    return {
        "gitRevision": revision,
        "gitDirty": bool(subprocess.run(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True,
            capture_output=True, check=False).stdout),
        "python": sys.version,
        "librarySha256": digest_path(ROOT / "build/libndn-service-framework.so"),
        "bindingSha256": digest_path(next((ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so"))),
    }


def validate_live_invoker(euid: int | None = None) -> None:
    if (os.geteuid() if euid is None else euid) != 0:
        raise PermissionError("formal Spec 129 confirmation requires one root owner")


def validate_campaign_not_frozen(*, dry_run: bool) -> None:
    if FORMAL_FREEZE_MARKER.is_file() and not dry_run:
        raise RuntimeError(
            "Spec 129 formal matrix is frozen and must not be rerun; "
            "define a new Spec for further evidence")


def command_for(cell: dict[str, Any], output: Path) -> list[str]:
    return [
        "sudo", "-n", "-E", "timeout", "300s", sys.executable,
        str(LAUNCHER), "--scenario", cell["id"], "--fault-profile",
        cell["faultProfile"], "--output", str(output),
    ]


def analyze_cell(cell: dict[str, Any], raw: dict[str, Any], return_code: int) -> dict[str, Any]:
    metrics = dict(raw.get("metrics") or {})
    missing = sorted(set(METRIC_SCHEMA) - set(metrics))
    checks = dict(raw.get("checks") or {})
    checks.update({
        "launcherExitZero": return_code == 0,
        "scenarioMatches": raw.get("scenario") == cell["id"],
        "metricSchemaComplete": not missing,
        "noProtectedPlaintext": int(metrics.get("packetPlaintextMatches", -1)) == 0,
        "zeroPreSelectionMutation": int(metrics.get("preSelectionMutationCount", -1)) == 0,
        "noUnselectedPreparation": int(metrics.get("unselectedPreparationCount", -1)) == 0,
        "noDuplicateExecution": int(metrics.get("duplicateExecutionCount", -1)) == 0,
        "singleInvocation": int(raw.get("invocationCount", 0)) == 1,
        "reservationDecisionAccounting": (
            int(metrics.get("reservationCreatedCount", -1)) ==
            int(metrics.get("decisionSelectedCount", -2)) +
            int(metrics.get("decisionNotSelectedCount", -2))),
        "noOrphanReservation": int(metrics.get("reservationCreatedCount", 0)) == 0
            or int(metrics.get("cleanupCount", -1)) >=
               int(metrics.get("reservationCreatedCount", 0)),
    })
    accepted = bool(raw.get("passed")) and all(checks.values())
    return {**cell, **metrics, "missingMetrics": missing, "checks": checks,
            "returnCode": return_code, "accepted": accepted,
            "terminalReason": raw.get("terminalReason", "")}


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    keys = ["cell", "id", "faultProfile", "accepted", "returnCode",
            "terminalReason", *METRIC_SCHEMA]
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    validate_campaign_not_frozen(dry_run=args.dry_run)
    output = Path(args.output_root).resolve()
    if "spec128" in output.name.lower():
        raise RuntimeError("Spec 128 evidence is immutable and cannot be a destination")
    if output.exists():
        raise RuntimeError("output reuse is forbidden for exact-once Spec 129 confirmation")

    cells = frozen_cells()
    validate_manifest(cells)
    source_hashes = hash_files(FROZEN_SOURCES)
    spec128_before = hash_spec128_evidence()
    identity = build_identity()
    output.mkdir(parents=True)
    lock_file = (output / ".campaign.lock").open("x+")
    fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    manifest = []
    for cell in cells:
        cell_output = output / f"cell-{cell['cell']:02d}-{cell['id']}"
        manifest.append({"cell": cell, "outputPath": str(cell_output),
                         "command": command_for(cell, cell_output),
                         "invocationCount": 0})
    (output / "campaign-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.dry_run:
        summary = {"status": "DRY_RUN", "createdAt": utc_now(),
                   "cellCount": 12, "sourceHashes": source_hashes,
                   "spec128HashesBefore": spec128_before,
                   "buildIdentity": identity}
        (output / "campaign-summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0

    validate_live_invoker()
    process_table = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,stat=,etimes=,user=,args="],
        text=True, capture_output=True, check=True).stdout
    owners = owner_processes(process_table, ignored_pids=process_ancestry())
    if owners:
        raise RuntimeError("another campaign owner exists: " + owners[0])

    rows = []
    for entry in manifest:
        if entry["invocationCount"] != 0:
            raise RuntimeError("cell invocation count is not zero before launch")
        cell_output = Path(entry["outputPath"])
        cell_output.mkdir()
        entry["invocationCount"] = 1
        (output / "campaign-manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        completed = subprocess.run(entry["command"], cwd=ROOT, text=True,
                                   capture_output=True)
        (cell_output / "runner.stdout.log").write_text(completed.stdout, encoding="utf-8")
        (cell_output / "runner.stderr.log").write_text(completed.stderr, encoding="utf-8")
        summary_path = cell_output / "cell-summary.json"
        raw = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.is_file() else {}
        rows.append(analyze_cell(entry["cell"], raw, completed.returncode))

    spec128_after = hash_spec128_evidence()
    spec128_unchanged = spec128_before == spec128_after
    write_csv(output / "campaign-runs.csv", rows)
    write_csv(output / "campaign-cells.csv", rows)
    summary = {
        "status": "PASS" if all(row["accepted"] for row in rows) and spec128_unchanged else "FAIL",
        "completedAt": utc_now(), "cellCount": 12,
        "acceptedCount": sum(bool(row["accepted"]) for row in rows),
        "failedCount": sum(not bool(row["accepted"]) for row in rows),
        "sourceHashes": source_hashes, "spec128HashesBefore": spec128_before,
        "spec128HashesAfter": spec128_after, "spec128Unchanged": spec128_unchanged,
        "buildIdentity": identity,
        "cells": rows,
    }
    (output / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"SPEC129_RUNNER_ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
