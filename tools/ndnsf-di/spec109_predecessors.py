#!/usr/bin/env python3
"""Fail-closed Spec 109 predecessor, deployment, and authority boundaries."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
from typing import Any, Mapping


REQUIRED_TASK_IDS = tuple(
    [f"107:T{i:03d}" for i in range(27, 39)]
    + [f"108:T{i:03d}" for i in range(91, 103)]
)
SHA256_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
PROTECTED_SPEC_PREFIXES = ("106-", "107-", "108-")
PROTECTED_RESULT_PREFIXES = ("spec106-", "spec107-", "spec108-")


class PredecessorError(ValueError):
    """Stable validation error used by CLI and tests."""


def _fail(code: str, detail: str = "") -> None:
    raise PredecessorError(code + (f":{detail}" if detail else ""))


def canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def digest_object(value: object) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def digest_file(path: Path | str) -> str:
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        _fail("PREDECESSOR_ARTIFACT_UNREADABLE", str(exc))
    return "sha256:" + digest.hexdigest()


def _json_object(path: Path | str) -> dict[str, Any]:
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except FileNotFoundError:
        _fail("PREDECESSOR_GATE_MISSING", str(path))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _fail("PREDECESSOR_GATE_INVALID", str(exc))
    if not isinstance(value, dict):
        _fail("PREDECESSOR_GATE_INVALID", "root-not-object")
    return value


def _safe_relative(value: object, root: Path) -> tuple[str, Path]:
    if not isinstance(value, str) or not value or "\\" in value:
        _fail("PREDECESSOR_ARTIFACT_PATH_INVALID", repr(value))
    pure = PurePosixPath(value)
    if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
        _fail("PREDECESSOR_ARTIFACT_PATH_INVALID", value)
    resolved = (root / Path(*pure.parts)).resolve(strict=False)
    try:
        resolved.relative_to(root)
    except ValueError:
        _fail("PREDECESSOR_ARTIFACT_PATH_INVALID", value)
    return pure.as_posix(), resolved


def _digest(value: object, field: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        _fail("PREDECESSOR_DIGEST_INVALID", field)
    return value


def verify_predecessor_gate(
    gate_path: Path | str, *, repo_root: Path | str | None = None
) -> dict[str, Any]:
    """Verify the exact predecessor task set, records, files, and gate digest."""

    root = Path(repo_root or Path(__file__).resolve().parents[2]).resolve()
    payload = _json_object(gate_path)
    if payload.get("schemaVersion") != "1.0":
        _fail("PREDECESSOR_SCHEMA_INVALID", repr(payload.get("schemaVersion")))
    required = payload.get("requiredTaskIds")
    if required != list(REQUIRED_TASK_IDS):
        _fail("PREDECESSOR_TASK_SET_INVALID", "requiredTaskIds")
    entries = payload.get("entries")
    if not isinstance(entries, dict) or set(entries) != set(REQUIRED_TASK_IDS):
        _fail("PREDECESSOR_TASK_SET_INVALID", "entries")
    gate_digest = _digest(payload.get("gateDigest"), "gateDigest")
    without_digest = dict(payload)
    del without_digest["gateDigest"]
    if digest_object(without_digest) != gate_digest:
        _fail("PREDECESSOR_GATE_DIGEST_MISMATCH")

    verified = []
    for task_id in REQUIRED_TASK_IDS:
        row = entries[task_id]
        if not isinstance(row, dict):
            _fail("PREDECESSOR_ENTRY_INVALID", task_id)
        expected_fields = {
            "requiredStatus", "observedStatus", "schemaVersion", "artifactPath",
            "artifactDigest", "identityDigest",
        }
        if set(row) != expected_fields:
            _fail("PREDECESSOR_ENTRY_FIELDS_INVALID", task_id)
        if row.get("requiredStatus") != "PASS" or row.get("observedStatus") != "PASS":
            _fail("PREDECESSOR_STATUS_NOT_PASS", task_id)
        schema = row.get("schemaVersion")
        if not isinstance(schema, str) or not schema.strip():
            _fail("PREDECESSOR_ENTRY_SCHEMA_INVALID", task_id)
        relative, artifact = _safe_relative(row.get("artifactPath"), root)
        expected_artifact = _digest(row.get("artifactDigest"), f"{task_id}.artifactDigest")
        identity = _digest(row.get("identityDigest"), f"{task_id}.identityDigest")
        if not artifact.is_file():
            _fail("PREDECESSOR_ARTIFACT_MISSING", relative)
        actual_artifact = digest_file(artifact)
        if actual_artifact != expected_artifact:
            _fail("PREDECESSOR_ARTIFACT_DIGEST_MISMATCH", task_id)
        verified.append({
            "taskId": task_id,
            "schemaVersion": schema,
            "artifactPath": relative,
            "artifactDigest": actual_artifact,
            "identityDigest": identity,
        })
    return {
        "schemaVersion": "1.0-verification",
        "status": "PASS",
        "gateDigest": gate_digest,
        "verifiedTaskCount": len(verified),
        "entries": verified,
    }


def observe_predecessor_lock(lock_path: Path | str) -> dict[str, Any]:
    """Produce a terminal, non-authoritative observation without rewriting predecessors."""
    payload = _json_object(lock_path)
    required = payload.get("requiredTaskIds")
    entries = payload.get("entries")
    if required != list(REQUIRED_TASK_IDS) or not isinstance(entries, dict) or set(entries) != set(REQUIRED_TASK_IDS):
        _fail("PREDECESSOR_TASK_SET_INVALID")
    observed = {}
    for task_id in REQUIRED_TASK_IDS:
        row = entries[task_id]
        if not isinstance(row, dict) or row.get("requiredStatus") != "PASS":
            _fail("PREDECESSOR_ENTRY_INVALID", task_id)
        status = str(row.get("observedStatus", ""))
        if status not in {"PASS", "FAIL", "INCOMPLETE", "BLOCKED"}:
            _fail("PREDECESSOR_STATUS_INVALID", task_id)
        observed[task_id] = {
            "requiredStatus": "PASS", "observedStatus": status,
            "schemaVersion": str(row.get("schemaVersion", "")),
            "artifactPath": row.get("artifactPath"),
            "artifactDigest": row.get("artifactDigest"),
            "identityDigest": row.get("identityDigest"),
        }
    missing = [task for task, row in observed.items() if row["observedStatus"] != "PASS"]
    core = {
        "schemaVersion": "1.0-observation", "status": "PASS" if not missing else "BLOCKED",
        "reasonCode": "" if not missing else "PREDECESSOR_TASKS_INCOMPLETE",
        "requiredTaskIds": list(REQUIRED_TASK_IDS), "entries": observed,
        "blockingTaskIds": missing, "sourceLockDigest": payload.get("lockDigest"),
        "sourceSnapshotDigest": payload.get("sourceSnapshotDigest"),
        "jobSubmitted": False, "physicalProduction": "DEFERRED",
    }
    core["observationDigest"] = digest_object(core)
    return core


def verify_deployment_profile(
    profile_path: Path | str, expected_digest: str
) -> dict[str, str]:
    """Bind a profile to its exact bytes before rendering or submitting."""

    expected = _digest(expected_digest, "deploymentProfileDigest")
    actual = digest_file(profile_path)
    if actual != expected:
        _fail("DEPLOYMENT_PROFILE_DIGEST_MISMATCH")
    return {"status": "PASS", "path": str(profile_path), "digest": actual}


def assert_spec109_mutation_allowed(
    target: Path | str, *, repo_root: Path | str | None = None
) -> Path:
    """Reject lexical and symlinked writes into Specs 106-108 or their results."""

    root = Path(repo_root or Path(__file__).resolve().parents[2]).resolve()
    raw = Path(target)
    lexical = raw if raw.is_absolute() else root / raw
    resolved = lexical.resolve(strict=False)
    candidates = (lexical, resolved)
    for candidate in candidates:
        try:
            parts = candidate.relative_to(root).parts
        except ValueError:
            _fail("SPEC109_MUTATION_DENIED", str(target))
        if len(parts) >= 2 and parts[0] == "specs" and parts[1].startswith(PROTECTED_SPEC_PREFIXES):
            _fail("SPEC109_MUTATION_DENIED", str(target))
        if len(parts) >= 2 and parts[0] == "results" and parts[1].startswith(PROTECTED_RESULT_PREFIXES):
            _fail("SPEC109_MUTATION_DENIED", str(target))
    return resolved


def validate_authority(value: Mapping[str, object]) -> dict[str, str]:
    expected = {"substrate", "candidate", "physicalProduction"}
    if set(value) != expected:
        _fail("AUTHORITY_FIELDS_INVALID")
    result = {key: str(value[key]) for key in expected}
    if result["substrate"] not in {"PASS", "FAIL", "INCONCLUSIVE", "BLOCKED"}:
        _fail("SUBSTRATE_AUTHORITY_INVALID")
    if result["candidate"] not in {"PASS", "FAIL", "INCONCLUSIVE", "BLOCKED", "DEFERRED"}:
        _fail("CANDIDATE_AUTHORITY_INVALID")
    if result["physicalProduction"] != "DEFERRED":
        _fail("PHYSICAL_PRODUCTION_AUTHORITY_INVALID")
    return result


__all__ = [
    "PredecessorError", "REQUIRED_TASK_IDS", "assert_spec109_mutation_allowed",
    "canonical_json", "digest_file", "digest_object", "observe_predecessor_lock", "validate_authority",
    "verify_deployment_profile", "verify_predecessor_gate",
]
