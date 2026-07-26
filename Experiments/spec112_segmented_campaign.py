"""Pure evidence and safety helpers for the Spec 112 MiniNDN campaign."""

from __future__ import annotations

import csv
import fcntl
import hashlib
import json
import os
import platform
import re
import socket
import shutil
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, TextIO

import spec112_candidate_manifest as candidate_tool


REPO = Path(__file__).resolve().parents[1]
SCHEMA = "spec112-segmented-cell-v1"
CANDIDATE_SCHEMA = "spec112-candidate-v1"
FAULT_PROFILES = ("none", "degraded-provider-after-targeted-bootstrap")
REFERENCE_MARKERS = (
    "LARGE_RESPONSE_REFERENCE_PUBLISHED",
    "NDNSF_RESPONSE_LARGE_REFERENCE",
    "LARGE_RESPONSE_REFERENCE_RESOLVED",
    "NDNSF-LARGE-DATA-REF",
)
MININDN_MARKER = re.compile(
    r"(?:MiniNDN|_Minindn\.py|_MiniNDN\.py|\bminindn\b|\bmnexec\b)", re.IGNORECASE
)
CSV_FIELDS = (
    "candidateId",
    "cellId",
    "status",
    "mode",
    "svsPublish",
    "faultProfile",
    "requestedCount",
    "resultCount",
    "passed",
    "failed",
    "userReturnCode",
    "providerReturnCode",
    "providerAlive",
    "userHung",
    "wallStop",
    "diskStop",
    "referenceMarkerCount",
    "providerEpoch",
    "elapsedSeconds",
    "resultPath",
)


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _atomic_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, raw = tempfile.mkstemp(prefix=f".{path.name}-", dir=str(path.parent))
    temporary = Path(raw)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            os.fchmod(output.fileno(), 0o644)
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(str(temporary), str(path))
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def load_candidate_manifest(
    path: Path,
    *,
    repo_root: Path = REPO,
    verify_current: bool = True,
) -> Dict[str, Any]:
    path = path.resolve()
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid candidate manifest: {path}") from exc
    if not isinstance(value, dict) or value.get("schemaVersion") != CANDIDATE_SCHEMA:
        raise ValueError("unsupported candidate manifest schema")
    candidate_id = value.get("candidateId")
    digest = value.get("identitySha256")
    if not isinstance(candidate_id, str) or not re.fullmatch(r"spec112-[0-9a-f]{20}", candidate_id):
        raise ValueError("invalid Spec 112 candidate ID")
    if path.name != "candidate-manifest.json" or path.parent.name != candidate_id:
        raise ValueError("candidate manifest path does not match candidate ID")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("invalid candidate identity digest")
    if not digest.startswith(candidate_id[len("spec112-"):]):
        raise ValueError("candidate ID is not derived from identity digest")
    if verify_current:
        current_identity = candidate_tool._identity(repo_root.resolve())
        current_digest = hashlib.sha256(candidate_tool._canonical_json(current_identity)).hexdigest()
        if current_digest != digest:
            raise ValueError(
                f"candidate identity changed: manifest={digest} current={current_digest}"
            )
        if value.get("identity") != current_identity:
            raise ValueError("candidate identity payload differs despite digest")
    return value


def reserve_cell_directory(output_dir: Path, candidate_manifest: Path) -> None:
    output_dir = output_dir.resolve()
    candidate_dir = candidate_manifest.resolve().parent
    if output_dir.parent != candidate_dir:
        raise ValueError("output directory must be a direct child of the candidate directory")
    if output_dir.name.startswith(".") or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", output_dir.name):
        raise ValueError("invalid evidence cell ID")
    try:
        output_dir.mkdir()
    except FileExistsError as exc:
        raise RuntimeError(f"evidence cell already exists: {output_dir}") from exc


def process_snapshot() -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for directory in Path("/proc").iterdir():
        if not directory.name.isdigit():
            continue
        try:
            cmdline = (directory / "cmdline").read_bytes().replace(b"\0", b" ").decode(
                "utf-8", errors="replace"
            ).strip()
            stat_fields = (directory / "stat").read_text(encoding="utf-8").split()
            ppid = int(stat_fields[3])
        except (OSError, ValueError, IndexError):
            continue
        rows.append({"pid": int(directory.name), "ppid": ppid, "cmdline": cmdline})
    return rows


def _ancestor_pids(rows: Iterable[Mapping[str, Any]], own_pid: int) -> set[int]:
    parent_by_pid = {int(row["pid"]): int(row.get("ppid", 0)) for row in rows}
    ancestors = {own_pid}
    current = own_pid
    while parent_by_pid.get(current, 0) > 0 and parent_by_pid[current] not in ancestors:
        current = parent_by_pid[current]
        ancestors.add(current)
    return ancestors


def find_conflicting_minindn_owners(
    rows: Iterable[Mapping[str, Any]], *, own_pid: int
) -> List[Dict[str, Any]]:
    materialized = [dict(row) for row in rows]
    excluded = _ancestor_pids(materialized, own_pid)
    conflicts = []
    for row in materialized:
        pid = int(row.get("pid", -1))
        cmdline = str(row.get("cmdline", ""))
        if pid not in excluded and cmdline and MININDN_MARKER.search(cmdline):
            conflicts.append({"pid": pid, "ppid": int(row.get("ppid", 0)), "cmdline": cmdline})
    return sorted(conflicts, key=lambda item: item["pid"])


def acquire_minindn_ownership(
    lock_path: Path,
    owner_path: Path,
    *,
    process_rows: Optional[Iterable[Mapping[str, Any]]] = None,
    own_pid: Optional[int] = None,
) -> TextIO:
    own_pid = os.getpid() if own_pid is None else own_pid
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock = lock_path.open("a+", encoding="utf-8")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise RuntimeError(f"MiniNDN ownership lock is held: {lock_path}") from exc
    rows = list(process_snapshot() if process_rows is None else process_rows)
    conflicts = find_conflicting_minindn_owners(rows, own_pid=own_pid)
    if conflicts:
        lock.close()
        raise RuntimeError(f"live external MiniNDN owner detected: {conflicts}")
    owner = {
        "schemaVersion": "spec112-minindn-owner-v1",
        "pid": own_pid,
        "hostname": socket.gethostname(),
        "startedAt": _utc_now(),
        "lockPath": str(lock_path.resolve()),
        "command": " ".join(os.environ.get("SPEC112_LAUNCH_COMMAND", "").split()),
        "externalOwners": conflicts,
    }
    _atomic_json(owner_path, owner)
    lock.seek(0)
    lock.truncate()
    lock.write(json.dumps(owner, sort_keys=True) + "\n")
    lock.flush()
    os.fsync(lock.fileno())
    return lock


def role_environment(
    base: Mapping[str, str],
    repo_root: Path = REPO,
    *,
    svs_sync_publish: bool = False,
) -> Dict[str, str]:
    env = dict(base)
    library_parts = [str(repo_root.parent / "ndn-svs/build"), str(repo_root / "build")]
    if env.get("LD_LIBRARY_PATH"):
        library_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(library_parts)
    env["NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE"] = "1"
    env["SPEC112_FORCED_INLINE_SVS"] = "1"
    # The runtime defaults to synchronous publication; both sides of the
    # boundary matrix must therefore be forced explicitly.
    env["NDNSF_SVS_ASYNC_PUBLISH"] = "0" if svs_sync_publish else "1"
    env["PYTHONPATH"] = str(repo_root / "pythonWrapper")
    env["NDNSF_BINARY_DIR"] = str(repo_root / "build/examples")
    env["NDNSF_LIBRARY_DIR"] = str(repo_root / "build")
    return env


def expand_sizes(value: str) -> List[int]:
    sizes: List[int] = []
    for raw in value.split(","):
        item = raw.strip().lower()
        if not item:
            continue
        parts = item.split("x", 1)
        try:
            size = int(parts[0])
            count = int(parts[1]) if len(parts) == 2 else 1
        except ValueError as exc:
            raise ValueError(f"invalid size sequence item: {raw!r}") from exc
        if size < 1 or count < 1:
            raise ValueError("sizes and counts must be positive")
        if len(sizes) + count > 10_000:
            raise ValueError("expanded size sequence exceeds 10000 requests")
        sizes.extend([size] * count)
    if not sizes:
        raise ValueError("at least one response size is required")
    return sizes


def execution_plan(sizes: str, mode: str, fault_profile: str) -> Dict[str, Any]:
    if fault_profile not in FAULT_PROFILES:
        raise ValueError(f"unknown fault profile: {fault_profile}")
    expanded = expand_sizes(sizes)
    pause_after: Optional[int] = None
    if fault_profile == "degraded-provider-after-targeted-bootstrap":
        if mode != "targeted":
            raise ValueError("degraded Provider fault profile requires Targeted mode")
        if len(expanded) == 1:
            expanded.append(expanded[0])
        pause_after = 0
    return {
        "requestedSizes": sizes,
        "expandedSizes": expanded,
        "effectiveSizes": ",".join(str(size) for size in expanded),
        "faultProfile": fault_profile,
        "pauseAfterIndex": pause_after,
    }


def verify_zero_loss_topology(path: Path) -> Dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    configured = [float(match) for match in re.findall(r"(?:^|\s)loss=([0-9]+(?:\.[0-9]+)?)", text)]
    if any(value != 0.0 for value in configured):
        raise ValueError(f"Spec 112 requires 0% configured loss: {path}")
    return {
        "path": str(path.resolve()),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "configuredLossPercent": configured or [0.0],
    }


def resource_stop_reason(
    output_dir: Path,
    *,
    started_monotonic: float,
    now_monotonic: float,
    wall_timeout_seconds: float,
    min_free_bytes: int,
) -> Optional[str]:
    if now_monotonic - started_monotonic >= wall_timeout_seconds:
        return "wall-timeout"
    if shutil.disk_usage(str(output_dir)).free < min_free_bytes:
        return "disk-floor"
    return None


def no_reference_proof(cell_dir: Path, *, forced_value: str) -> Dict[str, Any]:
    matches: List[Dict[str, Any]] = []
    inspected = []
    for name in ("controller.log", "provider.log", "user.log"):
        path = cell_dir / name
        if not path.is_file():
            continue
        inspected.append(name)
        text = path.read_text(encoding="utf-8", errors="replace")
        for marker in REFERENCE_MARKERS:
            count = text.count(marker)
            if count:
                matches.append({"file": name, "marker": marker, "count": count})
    marker_count = sum(int(item["count"]) for item in matches)
    return {
        "forcedEnvironment": {
            "NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE": forced_value,
            "SPEC112_FORCED_INLINE_SVS": "1",
        },
        "inspectedLogs": inspected,
        "markers": matches,
        "markerCount": marker_count,
        "verified": forced_value == "1" and marker_count == 0 and "provider.log" in inspected and "user.log" in inspected,
    }


def make_cell_summary(
    *,
    candidate_id: str,
    cell_id: str,
    mode: str,
    svs_publish: str,
    fault_profile: str,
    requested_sizes: List[int],
    results: List[Dict[str, Any]],
    owner: Dict[str, Any],
    provider_epoch: Dict[str, Any],
    no_reference: Dict[str, Any],
    user_return_code: Optional[int],
    provider_return_code: Optional[int],
    provider_alive: bool,
    user_hung: bool,
    wall_stop: bool,
    disk_stop: bool,
    elapsed_seconds: float,
) -> Dict[str, Any]:
    passed = sum(bool(result.get("ok")) for result in results)
    failed = len(results) - passed
    # A healthy long-running Provider has no exit status yet (poll() is None).
    # Treat that as the expected state; a non-None status is accepted only when
    # teardown has already produced one of the known clean exits.
    clean_provider_exit = provider_return_code is None or provider_return_code in (0, 130, -2)
    if fault_profile == "degraded-provider-after-targeted-bootstrap":
        expected_results = len(requested_sizes)
        fault_shape_ok = (
            len(results) == expected_results
            and bool(results and results[0].get("ok"))
            and bool(len(results) > 1 and not results[1].get("ok"))
            and bool(len(results) > 1 and results[1].get("deadlineWithinLimit"))
            and int(results[1].get("timeoutTerminalCount", 0)) == 1
        )
        accepted = (
            fault_shape_ok
            and not user_hung
            and not wall_stop
            and not disk_stop
            and bool(no_reference.get("verified"))
        )
    else:
        accepted = (
            len(results) == len(requested_sizes)
            and passed == len(requested_sizes)
            and user_return_code == 0
            and provider_alive
            and clean_provider_exit
            and not user_hung
            and not wall_stop
            and not disk_stop
            and bool(no_reference.get("verified"))
        )
    return {
        "schemaVersion": SCHEMA,
        "candidateId": candidate_id,
        "cellId": cell_id,
        "status": "SUCCESS" if accepted else "FAILURE",
        "mode": mode,
        "svsPublish": svs_publish,
        "faultProfile": fault_profile,
        "requestedSizes": requested_sizes,
        "requestedCount": len(requested_sizes),
        "resultCount": len(results),
        "passed": passed,
        "failed": failed,
        "results": results,
        "owner": owner,
        "providerEpoch": provider_epoch,
        "noReferenceProof": no_reference,
        "userReturnCode": user_return_code,
        "providerReturnCode": provider_return_code,
        "providerAlive": provider_alive,
        "userHung": user_hung,
        "wallStop": wall_stop,
        "diskStop": disk_stop,
        "elapsedSeconds": round(elapsed_seconds, 3),
        "completedAt": _utc_now(),
    }


def _csv_row(summary: Mapping[str, Any], result_path: Path) -> Dict[str, Any]:
    return {
        "candidateId": summary["candidateId"],
        "cellId": summary["cellId"],
        "status": summary["status"],
        "mode": summary["mode"],
        "svsPublish": summary["svsPublish"],
        "faultProfile": summary["faultProfile"],
        "requestedCount": summary["requestedCount"],
        "resultCount": summary["resultCount"],
        "passed": summary["passed"],
        "failed": summary["failed"],
        "userReturnCode": summary["userReturnCode"],
        "providerReturnCode": summary["providerReturnCode"],
        "providerAlive": int(bool(summary["providerAlive"])),
        "userHung": int(bool(summary["userHung"])),
        "wallStop": int(bool(summary["wallStop"])),
        "diskStop": int(bool(summary["diskStop"])),
        "referenceMarkerCount": summary["noReferenceProof"]["markerCount"],
        "providerEpoch": json.dumps(summary["providerEpoch"], sort_keys=True, separators=(",", ":")),
        "elapsedSeconds": summary["elapsedSeconds"],
        "resultPath": str(result_path.resolve()),
    }


def write_cell_and_candidate_summaries(
    cell_dir: Path,
    candidate_manifest: Path,
    summary: Dict[str, Any],
) -> None:
    candidate_dir = candidate_manifest.resolve().parent
    cell_summary_path = cell_dir / "cell-summary.json"
    aggregate_path = candidate_dir / "campaign-summary.json"
    csv_path = candidate_dir / "campaign-cells.csv"
    lock_path = candidate_dir / ".campaign-summary.lock"
    with lock_path.open("a+", encoding="utf-8") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        if aggregate_path.is_file():
            aggregate = json.loads(aggregate_path.read_text(encoding="utf-8"))
        else:
            aggregate = {
                "schemaVersion": "spec112-segmented-campaign-v1",
                "candidateId": summary["candidateId"],
                "manifestPath": str(candidate_manifest.resolve()),
                "cells": [],
            }
        if any(cell.get("cellId") == summary["cellId"] for cell in aggregate["cells"]):
            raise RuntimeError(f"cell is already recorded: {summary['cellId']}")
        if cell_summary_path.exists():
            raise RuntimeError(f"cell summary already exists: {cell_summary_path}")
        row = _csv_row(summary, cell_summary_path)
        rows: List[Dict[str, Any]] = []
        if csv_path.is_file():
            with csv_path.open(newline="", encoding="utf-8") as source:
                rows.extend(csv.DictReader(source))
        if any(row_item.get("cellId") == summary["cellId"] for row_item in rows):
            raise RuntimeError(f"cell CSV row is already recorded: {summary['cellId']}")
        rows.append(row)
        aggregate["cells"].append({**row, "summary": summary})
        aggregate["cellCount"] = len(aggregate["cells"])
        aggregate["acceptedCells"] = sum(cell["status"] == "SUCCESS" for cell in aggregate["cells"])
        aggregate["status"] = (
            "SUCCESS" if aggregate["acceptedCells"] == aggregate["cellCount"] else "FAILURE"
        )
        aggregate["updatedAt"] = _utc_now()

        _atomic_json(cell_summary_path, summary)
        descriptor, raw = tempfile.mkstemp(prefix=".campaign-cells-", dir=str(candidate_dir))
        temporary_csv = Path(raw)
        try:
            with os.fdopen(descriptor, "w", newline="", encoding="utf-8") as output:
                os.fchmod(output.fileno(), 0o644)
                writer = csv.DictWriter(output, fieldnames=CSV_FIELDS)
                writer.writeheader()
                writer.writerows(rows)
                output.flush()
                os.fsync(output.fileno())
            os.replace(str(temporary_csv), str(csv_path))
        except Exception:
            temporary_csv.unlink(missing_ok=True)
            raise
        _atomic_json(aggregate_path, aggregate)
