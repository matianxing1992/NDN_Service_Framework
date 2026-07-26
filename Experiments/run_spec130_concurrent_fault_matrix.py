#!/usr/bin/env python3
"""Single-writer exact-once Spec 130 16-cell MiniNDN confirmation runner."""

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
MANIFEST_SOURCE = ROOT / "specs/130-concurrent-fault-boundaries/experiment-manifest.json"
LAUNCHER = ROOT / "Experiments/NDNSF_DI_ConcurrentFaultBoundaries_Minindn.py"
SPEC129_SPEC = ROOT / "specs/129-selection-gated-deployment"
SPEC129_RESULT = ROOT / "results/spec129-r1-20260721_183058"
SPEC129_RUNNER_NAME = "run_spec129_selection_gated_deployment_matrix.py"
FROZEN_SOURCES = tuple(ROOT / name for name in (
    "NDNSF-DistributedInference/ndnsf_distributed_inference/core/conflict_coordination.py",
    "NDNSF-DistributedInference/ndnsf_distributed_inference/core/deployment_control.py",
    "NDNSF-DistributedInference/ndnsf_distributed_inference/deployment.py",
    "Experiments/NDNSF_DI_ConcurrentFaultBoundaries_Minindn.py",
    "Experiments/run_spec130_concurrent_fault_matrix.py",
    "specs/130-concurrent-fault-boundaries/experiment-manifest.json",
))

METRIC_SCHEMA = (
    "requestCount", "positiveAckCount", "negativeAckCount",
    "reservationCreatedCount", "reservationCommittedCount",
    "admissionGrantCount", "activationCount", "releaseCount", "expiryCount",
    "retryCount", "timeoutCount", "nackCount", "rejectionCount",
    "payloadDataCount", "mappingDataCount", "newMappingDataCount",
    "returnedNewMappingRatio", "payloadBytes", "mappingBytes",
    "safetyViolationCount", "orphanBeyondBoundCount", "staleRejectCount",
    "replayRejectCount", "tamperRejectCount", "splitBrainRejectCount",
    "completionCount", "boundedTerminationCount", "unavailableCount",
    "concurrentProgressCount", "falseSerializationCount", "blockingTimeMs",
    "completionLatencyMs", "p50LatencyMs", "p95LatencyMs",
    "eventCount", "packetPlaintextMatches",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def load_manifest(path: Path = MANIFEST_SOURCE) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def frozen_cells(manifest: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    source = manifest or load_manifest()
    values = [dict(value) for value in source["cells"]]
    next_cell = len(values) + 1
    statistics = source["statistics"]
    for seed in statistics["seeds"]:
        pair = f"S{int(seed)}"
        for mode in statistics["designs"]:
            values.append({
                "cell": next_cell, "pair": pair,
                "id": f"cycle-seed-{int(seed)}-{mode}",
                "graph": statistics["graph"], "mode": mode,
                "fault": statistics["fault"], "seed": int(seed),
                "expectedAvailability": "measured",
            })
            next_cell += 1
    return values


def validate_manifest(manifest: dict[str, Any]) -> None:
    cells = frozen_cells(manifest)
    if manifest.get("schemaVersion") != "spec130-confirmation-v2":
        raise RuntimeError("unsupported Spec 130 manifest schema")
    expected_count = int(manifest.get("expectedExpandedCellCount", 0))
    if [value.get("cell") for value in cells] != list(range(1, expected_count + 1)):
        raise RuntimeError("Spec 130 manifest expanded cell order is invalid")
    if len({value.get("id") for value in cells}) != expected_count:
        raise RuntimeError("Spec 130 manifest contains duplicate cell IDs")
    if manifest.get("automaticRetry") or manifest.get("rerunAllowed"):
        raise RuntimeError("formal Spec 130 cells cannot retry or rerun automatically")
    if manifest.get("spec129Policy") != "hash-only-never-invoke":
        raise RuntimeError("Spec 129 boundary is not frozen")
    required_graphs = {"disjoint", "identical", "nested", "partial", "cycle"}
    if not required_graphs <= {value.get("graph") for value in cells}:
        raise RuntimeError("conflict graph coverage is incomplete")
    if {"centralized", "lease-only"} != {value.get("mode") for value in cells}:
        raise RuntimeError("coordination comparison coverage is incomplete")
    stats = manifest.get("statistics") or {}
    seeds = stats.get("seeds") or []
    if len(seeds) != 22 or len(set(seeds)) != len(seeds):
        raise RuntimeError("stochastic seed plan is not independently frozen")
    pairs: dict[str, set[str]] = {}
    for cell in cells:
        pairs.setdefault(str(cell.get("pair", "")), set()).add(cell["mode"])
    if not pairs or any(modes != {"centralized", "lease-only"}
                        for modes in pairs.values()):
        raise RuntimeError("every deterministic and stochastic case must be paired")
    serialized = json.dumps(manifest, sort_keys=True).lower()
    if any(token in serialized for token in ("run_spec129", "uav", "codec")):
        raise RuntimeError("manifest crosses a frozen or workload-specific boundary")


def digest_path(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except PermissionError:
        completed = subprocess.run(["sudo", "-n", "sha256sum", "--", str(path)],
                                   text=True, capture_output=True, check=True)
        return completed.stdout.split()[0]


def hash_files(paths: Iterable[Path]) -> dict[str, str]:
    values = {}
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"missing frozen source: {path}")
        values[str(path.relative_to(ROOT))] = digest_path(path)
    return values


def hash_tree(path: Path) -> dict[str, str]:
    if not path.is_dir():
        raise RuntimeError(f"missing immutable tree: {path}")
    return {str(item.relative_to(ROOT)): digest_path(item)
            for item in sorted(path.rglob("*")) if item.is_file()}


def hash_spec129_evidence() -> dict[str, str]:
    values = hash_tree(SPEC129_SPEC)
    values.update(hash_tree(SPEC129_RESULT))
    return values


def process_ancestry(pid: int | None = None) -> set[int]:
    current = os.getpid() if pid is None else int(pid)
    values = set()
    while current > 1 and current not in values:
        values.add(current)
        try:
            current = int(Path(f"/proc/{current}/stat").read_text(
                encoding="utf-8").split()[3])
        except (FileNotFoundError, IndexError, ValueError):
            break
    return values


def owner_processes(process_table: str,
                    ignored_pids: set[int] | None = None) -> list[str]:
    ignored = set(ignored_pids or ()) | {os.getpid()}
    tokens = (LAUNCHER.name, Path(__file__).name, "minindn")
    values = []
    for line in process_table.splitlines():
        fields = line.split(None, 1)
        if fields and fields[0].isdigit() and int(fields[0]) in ignored:
            continue
        if SPEC129_RUNNER_NAME in line:
            values.append(line)
        elif any(token in line for token in tokens):
            values.append(line)
    return values


def validate_live_invoker(euid: int | None = None) -> None:
    if (os.geteuid() if euid is None else euid) != 0:
        raise PermissionError("formal Spec 130 confirmation requires one root owner")


def build_identity() -> dict[str, Any]:
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                              text=True, capture_output=True,
                              check=False).stdout.strip()
    library = ROOT / "build/libndn-service-framework.so"
    bindings = sorted((ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so"))
    if not library.is_file() or not bindings:
        raise RuntimeError("full source and Python binding build must precede Spec 130")
    return {"gitRevision": revision,
            "gitDirty": bool(subprocess.run(
                ["git", "status", "--porcelain"], cwd=ROOT, text=True,
                capture_output=True, check=False).stdout),
            "python": sys.version,
            "librarySha256": digest_path(library),
            "bindingSha256": digest_path(bindings[0])}


def command_for(cell: dict[str, Any], output: Path,
                manifest_digest: str) -> list[str]:
    return ["sudo", "-n", "-E", "timeout", "300s", sys.executable,
            str(LAUNCHER), "--scenario", cell["id"],
            "--graph", cell["graph"], "--mode", cell["mode"],
            "--fault", cell["fault"], "--manifest-digest", manifest_digest,
            "--seed", str(int(cell.get("seed", 0))), "--output", str(output)]


def expected_availability(cell: dict[str, Any], manifest: dict[str, Any]) -> str:
    del manifest
    return str(cell["expectedAvailability"])


def analyze_cell(cell: dict[str, Any], raw: dict[str, Any], return_code: int,
                 manifest: dict[str, Any] | None = None) -> dict[str, Any]:
    manifest = manifest or load_manifest()
    metrics = dict(raw.get("metrics") or {})
    missing = sorted(set(METRIC_SCHEMA) - set(metrics))
    mapping_count = int(metrics.get("mappingDataCount", 0))
    ratio = (int(metrics.get("newMappingDataCount", 0)) / mapping_count
             if mapping_count else 0.0)
    ratio_matches = abs(float(metrics.get("returnedNewMappingRatio", -1)) - ratio) < 1e-9
    availability = "unavailable" if int(metrics.get("unavailableCount", 0)) else "available"
    stochastic = cell["fault"] == "seeded-contention"
    checks = dict(raw.get("checks") or {})
    checks.update({
        "launcherExitZero": return_code == 0,
        "scenarioMatches": raw.get("scenario") == cell["id"],
        "manifestBound": raw.get("manifestDigest") == digest_path(MANIFEST_SOURCE),
        "metricSchemaComplete": not missing,
        "singleInvocation": int(raw.get("invocationCount", 0)) == 1,
        "zeroSafetyViolation": int(metrics.get("safetyViolationCount", -1)) == 0,
        "zeroOrphanBeyondBound": int(metrics.get("orphanBeyondBoundCount", -1)) == 0,
        "zeroProtectedPlaintext": int(metrics.get("packetPlaintextMatches", -1)) == 0,
        "newMappingRatioValid": ratio_matches,
        "availabilityClassMatches": (stochastic or availability ==
                                      expected_availability(cell, manifest)),
    })
    accepted = bool(raw.get("passed")) and all(checks.values())
    return {**cell, **metrics, "availabilityOutcome": availability,
            "missingMetrics": missing, "checks": checks,
            "returnCode": return_code, "accepted": accepted,
            "terminalReason": raw.get("terminalReason", "")}


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    keys = ["cell", "id", "graph", "mode", "fault", "accepted",
            "availabilityOutcome", "returnCode", "terminalReason", *METRIC_SCHEMA]
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=keys, extrasaction="ignore")
        writer.writeheader(); writer.writerows(rows)


def write_analysis(path: Path, rows: list[dict[str, Any]]) -> None:
    total_mapping = sum(int(row["mappingDataCount"]) for row in rows)
    total_new = sum(int(row["newMappingDataCount"]) for row in rows)
    value = {
        "payload": {"dataCount": sum(int(row["payloadDataCount"]) for row in rows),
                    "bytes": sum(int(row["payloadBytes"]) for row in rows)},
        "mapping": {"dataCount": total_mapping,
                    "bytes": sum(int(row["mappingBytes"]) for row in rows),
                    "newInformationCount": total_new,
                    "returnedNewInformationRatio": total_new / total_mapping
                    if total_mapping else 0.0},
        "retryCount": sum(int(row["retryCount"]) for row in rows),
        "timeoutCount": sum(int(row["timeoutCount"]) for row in rows),
        "nackCount": sum(int(row["nackCount"]) for row in rows),
        "rejectionCount": sum(int(row["rejectionCount"]) for row in rows),
        "safetyViolationCount": sum(int(row["safetyViolationCount"]) for row in rows),
        "unavailableCellCount": sum(row["availabilityOutcome"] == "unavailable"
                                    for row in rows),
        "centralized": [row for row in rows if row["mode"] == "centralized"],
        "leaseOnly": [row for row in rows if row["mode"] == "lease-only"],
    }
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--require-minindn", action="store_true")
    args = parser.parse_args()
    output = Path(args.output_root).resolve()
    if any(value in output.name.lower() for value in ("spec129", "spec128")):
        raise RuntimeError("frozen prior-Spec namespace cannot be a destination")
    if output.exists():
        raise RuntimeError("output reuse is forbidden for exact-once Spec 130 confirmation")
    manifest_source = load_manifest(); validate_manifest(manifest_source)
    manifest_digest = digest_path(MANIFEST_SOURCE)
    source_hashes = hash_files(FROZEN_SOURCES)
    spec129_before = hash_spec129_evidence()
    identity = build_identity()
    output.mkdir(parents=True)
    lock_file = (output / ".campaign.lock").open("x+")
    fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    manifest = []
    for cell in frozen_cells(manifest_source):
        cell_output = output / f"cell-{cell['cell']:02d}-{cell['id']}"
        manifest.append({"cell": cell, "outputPath": str(cell_output),
                         "command": command_for(cell, cell_output, manifest_digest),
                         "invocationCount": 0})
    (output / "campaign-manifest.json").write_text(
        json.dumps({"manifestDigest": manifest_digest, "entries": manifest},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.dry_run:
        summary = {"status": "DRY_RUN", "createdAt": utc_now(),
                   "cellCount": len(manifest), "manifestDigest": manifest_digest,
                   "sourceHashes": source_hashes,
                   "spec129HashesBefore": spec129_before,
                   "buildIdentity": identity}
        (output / "campaign-summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0
    if not args.require_minindn:
        raise RuntimeError("live Spec 130 confirmation requires --require-minindn")
    validate_live_invoker()
    process_table = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,stat=,etimes=,user=,args="], text=True,
        capture_output=True, check=True).stdout
    owners = owner_processes(process_table, process_ancestry())
    if owners:
        raise RuntimeError("another campaign owner exists: " + owners[0])
    rows = []
    for entry in manifest:
        if entry["invocationCount"] != 0:
            raise RuntimeError("cell invocation count is not zero before launch")
        cell_output = Path(entry["outputPath"]); cell_output.mkdir()
        entry["invocationCount"] = 1
        (output / "campaign-manifest.json").write_text(
            json.dumps({"manifestDigest": manifest_digest, "entries": manifest},
                       indent=2, sort_keys=True) + "\n", encoding="utf-8")
        completed = subprocess.run(entry["command"], cwd=ROOT, text=True,
                                   capture_output=True)
        (cell_output / "runner.stdout.log").write_text(completed.stdout, encoding="utf-8")
        (cell_output / "runner.stderr.log").write_text(completed.stderr, encoding="utf-8")
        summary_path = cell_output / "cell-summary.json"
        raw = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.is_file() else {}
        rows.append(analyze_cell(entry["cell"], raw, completed.returncode,
                                 manifest_source))
    spec129_after = hash_spec129_evidence()
    unchanged = spec129_before == spec129_after
    write_csv(output / "campaign-runs.csv", rows)
    write_csv(output / "campaign-cells.csv", rows)
    write_analysis(output / "campaign-analysis.json", rows)
    summary = {"status": "PASS" if all(row["accepted"] for row in rows)
               and unchanged else "FAIL", "completedAt": utc_now(),
               "cellCount": len(manifest),
               "acceptedCount": sum(bool(row["accepted"]) for row in rows),
               "failedCount": sum(not bool(row["accepted"]) for row in rows),
               "manifestDigest": manifest_digest, "sourceHashes": source_hashes,
               "spec129HashesBefore": spec129_before,
               "spec129HashesAfter": spec129_after, "spec129Unchanged": unchanged,
               "buildIdentity": identity, "cells": rows}
    (output / "campaign-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001
        print(f"SPEC130_RUNNER_ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
