#!/usr/bin/env python3
"""Fail-closed offline evidence parser for Spec 137.

The module owns schema validation, treatment-diff admission, lifecycle
conservation, deterministic pilot rate selection, and hash verification.
Network execution stays in the MiniNDN runner; this file never starts or
repairs a cell.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
from typing import Any, Iterable


EVENT_SCHEMA = "spec137.event.v1"
RESOURCE_SCHEMA = "spec137.resources.v1"
RATES = (60,)
PEERS = ("peer-a", "peer-b")
MODES = ("face-serial", "worker-serial")
PHASES = {"startup", "warmup", "measured", "drain", "shutdown"}
THREAD_ROLES = {"main", "face", "production-worker", "supervisor"}
ALLOWED_TREATMENT_FIELDS = {
    "production_mode",
    "parallel_sync_production",
    "production_workers",
    "production_queue_capacity",
    "sign_in_worker",
    "build_extra_in_worker",
    "worker_cpu_active",
}
MANDATORY_ONCE = {
    "runtime-config",
    "ready",
    "signer-concurrency-max",
    "worker-stats",
    "shutdown-start",
    "shutdown-complete",
    "process-summary",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid JSON authority {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON authority must be an object: {path}")
    return value


def verify_hash(path: Path, expected: str, label: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"{label} is missing: {path}")
    actual = sha256_file(path)
    if actual != expected:
        raise RuntimeError(
            f"{label} hash mismatch: expected={expected} actual={actual}"
        )


def read_events(
    path: Path, campaign_id: str, cell_id: str, peer_id: str
) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise RuntimeError(f"cannot read event file {path}: {error}") from error
    if not lines:
        raise RuntimeError(f"event file is empty: {path}")
    required = {
        "schema",
        "campaignId",
        "cellId",
        "peerId",
        "phase",
        "event",
        "monotonicNs",
        "threadRole",
        "logicalId",
        "productionId",
        "details",
    }
    for number, line in enumerate(lines, 1):
        try:
            event = json.loads(line)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"{path}:{number}: invalid event JSON: {error}") from error
        if not isinstance(event, dict) or set(event) != required:
            raise RuntimeError(f"{path}:{number}: event field/schema mismatch")
        if (
            event["schema"] != EVENT_SCHEMA
            or event["campaignId"] != campaign_id
            or event["cellId"] != cell_id
            or event["peerId"] != peer_id
        ):
            raise RuntimeError(f"{path}:{number}: event identity/schema mismatch")
        if event["phase"] not in PHASES or event["threadRole"] not in THREAD_ROLES:
            raise RuntimeError(f"{path}:{number}: invalid phase/thread role")
        if (
            not isinstance(event["monotonicNs"], int)
            or event["monotonicNs"] < 0
        ):
            raise RuntimeError(f"{path}:{number}: invalid timestamp")
        if (
            not isinstance(event["logicalId"], int)
            or event["logicalId"] < 0
            or not isinstance(event["productionId"], int)
            or event["productionId"] < 0
            or not isinstance(event["details"], dict)
            or not isinstance(event["event"], str)
            or not event["event"]
        ):
            raise RuntimeError(f"{path}:{number}: invalid event value")
        events.append(event)
    return events


def read_resource(
    path: Path, campaign_id: str, cell_id: str, peer_id: str
) -> dict[str, Any]:
    value = load_json(path)
    if (
        value.get("schema") != RESOURCE_SCHEMA
        or value.get("campaignId") != campaign_id
        or value.get("cellId") != cell_id
        or value.get("peerId") != peer_id
    ):
        raise RuntimeError(f"resource identity/schema mismatch: {path}")
    for key in (
        "maxRssKiB",
        "voluntaryContextSwitches",
        "involuntaryContextSwitches",
    ):
        if not isinstance(value.get(key), int) or value[key] < 0:
            raise RuntimeError(f"invalid resource field {key}: {path}")
    return value


def event_by_name(events: list[dict[str, Any]], name: str) -> dict[str, Any]:
    selected = [event for event in events if event["event"] == name]
    if len(selected) != 1:
        raise RuntimeError(f"{name} count must equal one, got {len(selected)}")
    return selected[0]


def expanded_config(events: list[dict[str, Any]]) -> dict[str, Any]:
    return dict(event_by_name(events, "runtime-config")["details"])


def runtime_config_delta(
    face: dict[str, Any], worker: dict[str, Any]
) -> set[str]:
    if set(face) != set(worker):
        raise RuntimeError("expanded runtime configuration key set differs")
    return {key for key in face if face[key] != worker[key]}


def production_accounting_remainder(mode: str, stats: dict[str, Any]) -> int:
    if mode == "face-serial":
        return int(stats["triggers"]) - int(stats["serialCompleted"]) - int(
            stats["serialFailures"]
        )
    if mode != "worker-serial":
        raise RuntimeError(f"unknown production mode: {mode}")
    terminals = sum(
        int(stats[key])
        for key in (
            "completed",
            "staleDropped",
            "workerFailures",
            "faceFailures",
            "cancelled",
        )
    )
    return int(stats["submitted"]) - terminals


def publication_accounting_remainder(summary: dict[str, Any]) -> int:
    return (
        int(summary["scheduledMeasured"])
        - int(summary["attemptedMeasured"])
        - int(
            summary.get(
                "skippedMeasuredReleases", summary["skippedReleases"]
            )
        )
    )


def validate_peer_evidence(
    peer: dict[str, Any], target_rate: int
) -> dict[str, Any]:
    events = peer.get("events")
    resource = peer.get("resource")
    if not isinstance(events, list) or not isinstance(resource, dict):
        raise RuntimeError("peer evidence lacks events/resources")
    for name in MANDATORY_ONCE:
        event_by_name(events, name)
    if any(event["event"] == "production-terminal-anomaly" for event in events):
        raise RuntimeError("production-terminal-anomaly is present")
    config = expanded_config(events)
    mode = config.get("production_mode")
    if mode not in MODES:
        raise RuntimeError("runtime-config mode is invalid")
    if config.get("face_threads") != 1 or config.get("receive_workers") != 0:
        raise RuntimeError("runtime-config is not one-Face/no-receive-worker")
    if config.get("rate_pps") != target_rate:
        raise RuntimeError("runtime-config rate differs from target")
    if config.get("sync_interest_batching") is not False:
        raise RuntimeError("Sync Interest batching must be disabled")
    if config.get("parallel_sync_processing") is not False:
        raise RuntimeError("parallel receive processing must be disabled")
    if config.get("protocol") != "v2":
        raise RuntimeError("protocol must be V2")
    if mode == "face-serial":
        if (
            config.get("parallel_sync_production") is not False
            or config.get("production_workers") != 0
            or any(
                event["threadRole"] == "production-worker"
                for event in events
                if event["event"] != "worker-thread-config"
            )
        ):
            raise RuntimeError("face-serial unexpectedly uses a production worker")
    else:
        if (
            config.get("parallel_sync_production") is not True
            or config.get("production_workers") != 1
            or config.get("worker_cpu_active") is not True
        ):
            raise RuntimeError("worker-serial configuration is incomplete")
        worker_configs = [
            event for event in events if event["event"] == "worker-thread-config"
        ]
        if len(worker_configs) != 1 or not worker_configs[0]["details"].get(
            "pinned", False
        ):
            raise RuntimeError("worker-serial worker pin evidence is missing")

    stats = dict(event_by_name(events, "worker-stats")["details"])
    summary = dict(event_by_name(events, "process-summary")["details"])
    signer = event_by_name(events, "signer-concurrency-max")["details"]
    if set(peer) >= {"stats", "summary"}:
        if peer["stats"] != stats or peer["summary"] != summary:
            raise RuntimeError("peer derived stats/summary disagree with events")
    attempted = int(summary["attemptedMeasured"])
    expected = (
        target_rate * int(config["measure_s"])
        if config.get("publish_enabled", True)
        else 0
    )
    attempted_error = (
        abs(attempted - expected) / expected
        if expected
        else 0.0 if attempted == 0 else math.inf
    )
    production_remainder = production_accounting_remainder(mode, stats)
    publication_remainder = publication_accounting_remainder(summary)
    result = {
        "mode": mode,
        "config": config,
        "stats": stats,
        "summary": summary,
        "attemptedRateError": attempted_error,
        "maxActiveSigners": int(signer["value"]),
        "productionAccountingRemainder": production_remainder,
        "publicationAccountingRemainder": publication_remainder,
        "fallbacks": int(stats["fallbacks"]),
        "ownerViolations": int(stats["ownerViolations"]),
        "pending": int(stats["pending"]),
        "shutdownDrained": int(stats["pending"]) == 0
        and int(stats["activeSigners"]) == 0,
        "resourceComplete": all(
            key in resource
            for key in (
                "maxRssKiB",
                "voluntaryContextSwitches",
                "involuntaryContextSwitches",
            )
        ),
    }
    peer["stats"] = stats
    peer["summary"] = summary
    peer["validated"] = result
    return result


def admit_pair(
    face: dict[str, Any], worker: dict[str, Any], target_rate: int
) -> dict[str, bool]:
    left = validate_peer_evidence(face, target_rate)
    right = validate_peer_evidence(worker, target_rate)
    delta = runtime_config_delta(left["config"], right["config"])
    return {
        "same_runtime_controls_except_treatment": delta
        == ALLOWED_TREATMENT_FIELDS,
        "one_face_thread": left["config"]["face_threads"] == 1
        and right["config"]["face_threads"] == 1,
        "receive_workers_zero": left["config"]["receive_workers"] == 0
        and right["config"]["receive_workers"] == 0,
        "attempted_rate_within_2_percent": left["attemptedRateError"] <= 0.02
        and right["attemptedRateError"] <= 0.02,
        "max_active_sync_signers_one": left["maxActiveSigners"] == 1
        and right["maxActiveSigners"] == 1,
        "production_fallback_zero": left["fallbacks"] == 0
        and right["fallbacks"] == 0,
        "production_accounting_remainder_zero": left[
            "productionAccountingRemainder"
        ]
        == 0
        and right["productionAccountingRemainder"] == 0,
        "publication_accounting_remainder_zero": left[
            "publicationAccountingRemainder"
        ]
        == 0
        and right["publicationAccountingRemainder"] == 0,
        "thread_owner_valid": left["ownerViolations"] == 0
        and right["ownerViolations"] == 0,
        "event_and_resource_files_complete": left["resourceComplete"]
        and right["resourceComplete"],
        "shutdown_drained": left["shutdownDrained"] and right["shutdownDrained"],
    }


def _synthetic_event(
    mode: str, name: str, details: dict[str, Any], stamp: int
) -> dict[str, Any]:
    return {
        "schema": EVENT_SCHEMA,
        "campaignId": "synthetic",
        "cellId": "synthetic-cell",
        "peerId": "peer-a",
        "phase": "startup" if name in {"runtime-config", "ready", "worker-thread-config"} else "shutdown",
        "event": name,
        "monotonicNs": stamp,
        "threadRole": "production-worker"
        if name == "worker-thread-config"
        else "face",
        "logicalId": 0,
        "productionId": 0,
        "details": details,
    }


def synthetic_peer_evidence(mode: str) -> dict[str, Any]:
    worker = mode == "worker-serial"
    config = {
        "production_mode": mode,
        "parallel_sync_processing": False,
        "parallel_sync_production": worker,
        "face_threads": 1,
        "receive_workers": 0,
        "production_workers": 1 if worker else 0,
        "production_queue_capacity": 4096 if worker else 0,
        "sign_in_worker": worker,
        "build_extra_in_worker": worker,
        "worker_cpu_active": worker,
        "sync_interest_batching": False,
        "protocol": "v2",
        "sync_security": "hmac",
        "publication_security": "sha256",
        "publish_enabled": True,
        "payload_bytes": 256,
        "main_cpu": 0,
        "face_cpu": 1,
        "worker_cpu": 2,
        "rate_pps": 60,
        "warmup_s": 10,
        "measure_s": 60,
        "drain_s": 10,
    }
    stats = {
        "triggers": 100,
        "submitted": 100 if worker else 0,
        "serialCompleted": 100 if not worker else 0,
        "serialFailures": 0,
        "completed": 100 if worker else 0,
        "staleSent": 0,
        "staleDropped": 0,
        "workerFailures": 0,
        "faceFailures": 0,
        "cancelled": 0,
        "pending": 0,
        "fallbacks": 0,
        "activeSigners": 0,
        "maxActiveSigners": 1,
        "ownerViolations": 0,
        "workerThreadChanges": 0,
        "maxWorkerQueueDepth": 1 if worker else 0,
    }
    summary = {
        "scheduledMeasured": 3600,
        "attemptedMeasured": 3600,
        "skippedReleases": 0,
        "skippedMeasuredReleases": 0,
        "deliveredMeasured": 3600,
        "publishErrors": 0,
        "invalid": 0,
        "duplicates": 0,
        "heartbeatCount": 60000,
        "heartbeatP99Ns": 500000,
        "workerPinFailed": False,
    }
    ordered = [
        ("runtime-config", config),
        ("ready", {}),
    ]
    if worker:
        ordered.append(("worker-thread-config", {"cpu": 2, "pinned": True}))
    ordered.extend(
        [
            ("shutdown-start", {}),
            ("worker-stats", stats),
            ("signer-concurrency-max", {"value": 1}),
            ("process-summary", summary),
            ("shutdown-complete", {}),
        ]
    )
    events = [
        _synthetic_event(mode, name, details, index)
        for index, (name, details) in enumerate(ordered, 1)
    ]
    return {
        "events": events,
        "resource": {
            "schema": RESOURCE_SCHEMA,
            "campaignId": "synthetic",
            "cellId": "synthetic-cell",
            "peerId": "peer-a",
            "maxRssKiB": 100,
            "voluntaryContextSwitches": 1,
            "involuntaryContextSwitches": 0,
        },
        "stats": stats,
        "summary": summary,
    }


def pilot_fixture(
    rate: int,
    *,
    attempted_error: float = 0.0,
    delivery_ratio: float = 0.99,
    face_heartbeat_p99_ns: int = 500_000,
    worker_heartbeat_p99_ns: int | None = None,
    face_delivery_p99_ns: int = 20_000_000,
    worker_delivery_p99_ns: int | None = None,
) -> dict[str, Any]:
    worker_heartbeat = (
        int(face_heartbeat_p99_ns * 0.75)
        if worker_heartbeat_p99_ns is None
        else worker_heartbeat_p99_ns
    )
    worker_delivery = (
        int(face_delivery_p99_ns * 0.95)
        if worker_delivery_p99_ns is None
        else worker_delivery_p99_ns
    )
    common = {
        "attemptedRateError": attempted_error,
        "deliveryRatio": delivery_ratio,
        "accountingComplete": True,
        "resourceComplete": True,
        "fallbacks": 0,
        "productionAccountingRemainder": 0,
        "publicationAccountingRemainder": 0,
        "maxActiveSigners": 1,
    }
    return {
        "candidateRate": rate,
        "face": {
            **common,
            "heartbeatP99Ns": face_heartbeat_p99_ns,
            "deliveryP99Ns": face_delivery_p99_ns,
        },
        "worker": {
            **common,
            "heartbeatP99Ns": worker_heartbeat,
            "deliveryP99Ns": worker_delivery,
        },
    }


def mode_jointly_admissible(value: dict[str, Any]) -> bool:
    return (
        float(value["attemptedRateError"]) <= 0.02
        and float(value["deliveryRatio"]) >= 0.95
        and bool(value["accountingComplete"])
        and bool(value["resourceComplete"])
        and int(value["fallbacks"]) == 0
        and int(value["productionAccountingRemainder"]) == 0
        and int(value["publicationAccountingRemainder"]) == 0
        and int(value["maxActiveSigners"]) == 1
    )


def select_stress_rate(rows: Iterable[dict[str, Any]]) -> dict[str, Any]:
    evaluated: list[dict[str, Any]] = []
    seen: set[int] = set()
    for row in sorted(rows, key=lambda item: int(item["candidateRate"])):
        rate = int(row["candidateRate"])
        if rate not in RATES or rate in seen:
            raise RuntimeError(f"pilot rate is invalid or duplicated: {rate}")
        seen.add(rate)
        face, worker = row["face"], row["worker"]
        joint = mode_jointly_admissible(face) and mode_jointly_admissible(worker)
        condition_a = (
            int(face["heartbeatP99Ns"]) >= 1_000_000
            and int(worker["heartbeatP99Ns"])
            <= 0.80 * int(face["heartbeatP99Ns"])
        )
        condition_b = (
            float(worker["deliveryRatio"])
            >= float(face["deliveryRatio"]) + 0.01
            and int(worker["deliveryP99Ns"])
            <= 1.10 * int(face["deliveryP99Ns"])
        )
        evaluated.append(
            {
                **row,
                "faceAdmissible": mode_jointly_admissible(face),
                "workerAdmissible": mode_jointly_admissible(worker),
                "jointlyAdmissible": joint,
                "stressConditionA": condition_a,
                "stressConditionB": condition_b,
                "faceStressing": joint and (condition_a or condition_b),
            }
        )
    joint = [row for row in evaluated if row["jointlyAdmissible"]]
    if joint:
        selected, reason = RATES[0], "FIXED_RATE_ADMITTED"
    else:
        selected, reason = None, "FIXED_RATE_INADMISSIBLE"
    return {
        "schema": "spec137.rate-selection.v1",
        "selectedRate": selected,
        "reason": reason,
        "evaluated": evaluated,
    }


def parse_peer_directory(
    directory: Path,
    campaign_id: str,
    cell_id: str,
    peer_id: str,
    target_rate: int,
) -> dict[str, Any]:
    evidence = {
        "events": read_events(
            directory / f"{peer_id}-events.jsonl",
            campaign_id,
            cell_id,
            peer_id,
        ),
        "resource": read_resource(
            directory / f"{peer_id}-resources.json",
            campaign_id,
            cell_id,
            peer_id,
        ),
    }
    validate_peer_evidence(evidence, target_rate)
    return evidence


def verify_preflight(campaign: Path) -> dict[str, Any]:
    receipt = load_json(campaign / "preflight/preflight-summary.json")
    if receipt.get("schema") != "spec137.preflight.v1":
        raise RuntimeError("preflight summary schema mismatch")
    if receipt.get("formalReceiptsCreated") != 0:
        raise RuntimeError("preflight created a formal receipt")
    checks = receipt.get("checks")
    if not isinstance(checks, dict) or not checks or not all(checks.values()):
        raise RuntimeError(f"preflight admission failed: {checks}")
    for label, record in receipt.get("artifacts", {}).items():
        verify_hash(Path(record["path"]), record["sha256"], label)
    return receipt


def verify_campaign(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    preflight = verify_preflight(campaign)
    result: dict[str, Any] = {
        "schema": "spec137.analysis-verification.v1",
        "campaign": str(campaign),
        "preflightAdmitted": True,
        "formalReceiptCount": 0,
        "formalStarted": False,
    }
    manifest_path = campaign / "campaign-manifest.json"
    if manifest_path.exists():
        manifest = load_json(manifest_path)
        if manifest.get("schema") != "spec137.campaign.v1":
            raise RuntimeError("campaign manifest schema mismatch")
        cells = manifest.get("cells", [])
        expected = [
            (1, 1, "face-serial"),
            (2, 1, "worker-serial"),
            (3, 2, "worker-serial"),
            (4, 2, "face-serial"),
            (5, 3, "face-serial"),
            (6, 3, "worker-serial"),
        ]
        if [
            (cell.get("ordinal"), cell.get("pair"), cell.get("mode"))
            for cell in cells
        ] != expected:
            raise RuntimeError("formal campaign matrix changed")
        receipts = sorted((campaign / "receipts").glob("*.json"))
        ordinals = [load_json(path).get("ordinal") for path in receipts]
        if len(ordinals) != len(set(ordinals)):
            raise RuntimeError("duplicate formal receipt ordinal")
        result["formalReceiptCount"] = len(receipts)
        result["formalStarted"] = bool(receipts)
    result["preflightSummarySha256"] = sha256_file(
        campaign / "preflight/preflight-summary.json"
    )
    result["subjectManifestSha256"] = preflight["subjectManifestSha256"]
    return result


def write_report(path: Path, verification: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "# Spec 137 Serial Production Offload Verification\n\n"
        f"- Campaign: `{verification['campaign']}`\n"
        f"- Preflight admitted: `{verification['preflightAdmitted']}`\n"
        f"- Formal started: `{verification['formalStarted']}`\n"
        f"- Formal receipt count: `{verification['formalReceiptCount']}`\n"
        f"- Subject manifest SHA-256: `{verification['subjectManifestSha256']}`\n\n"
        "This verification is an evidence-integrity result. A causal outcome "
        "requires the sealed six-cell formal matrix and paired analysis.\n",
        encoding="utf-8",
    )


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = sorted({key for row in rows for key in row})
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.add_argument("--report", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    result = verify_campaign(args.campaign)
    if args.report is not None:
        write_report(args.report, result)
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
