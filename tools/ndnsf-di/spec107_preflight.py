#!/usr/bin/env python3
"""Fail-before-role-start preflight for Spec 107 campaigns."""

from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path, PurePosixPath
import shutil
from typing import Any, Mapping, Sequence

from spec107_identity import (
    IdentityError,
    digest_object,
    validate_campaign_set,
    validate_candidate_identity,
)
from spec107_lineage import assert_mutation_allowed, sha256_file


SCHEMA = "ndnsf-di-spec107-preflight-v1"
ARTIFACT_SCHEMA = "ndnsf-di-spec107-artifact-set-v1"
DEFAULT_RESERVE_BYTES = 1024 * 1024 * 1024
REQUIRED_CAPABILITY = "qwen-generation-session-v1"


class PreflightError(ValueError):
    """Stable fail-closed campaign-preflight error."""


def _fail(code: str, detail: str = "") -> None:
    suffix = f":{detail}" if detail else ""
    raise PreflightError(code + suffix)


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(
        microsecond=0).isoformat().replace("+00:00", "Z")


def _existing_ancestor(path: Path) -> Path:
    current = path
    while not current.exists() and current != current.parent:
        current = current.parent
    return current


def _process_exists(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _writer_reason(marker: Path) -> str | None:
    if not marker.exists():
        return None
    try:
        payload = json.loads(marker.read_text(encoding="utf-8"))
        pid = payload.get("pid")
    except (OSError, UnicodeError, json.JSONDecodeError):
        return "OUTPUT_STALE_WRITER"
    if isinstance(pid, bool) or not isinstance(pid, int):
        return "OUTPUT_STALE_WRITER"
    return "OUTPUT_ACTIVE_WRITER" if _process_exists(pid) else "OUTPUT_STALE_WRITER"


def _artifact_reasons(
    *,
    candidate_id: str,
    candidate_artifact_digest: str,
    artifact_root: Path,
    manifest: Mapping[str, object],
) -> list[str]:
    reasons: list[str] = []
    if manifest.get("schema") != ARTIFACT_SCHEMA:
        return ["ARTIFACT_MANIFEST_SCHEMA_INVALID"]
    manifest_candidate = manifest.get("candidateId")
    if manifest_candidate is not None and manifest_candidate != candidate_id:
        reasons.append("ARTIFACT_CANDIDATE_MISMATCH")
    manifest_path = artifact_root / "artifact-set.json"
    try:
        manifest_digest = sha256_file(manifest_path)
    except OSError:
        reasons.append("ARTIFACT_MANIFEST_UNREADABLE")
    else:
        if manifest_digest != candidate_artifact_digest:
            reasons.append("ARTIFACT_MANIFEST_CANDIDATE_DIGEST_MISMATCH")
    rows = manifest.get("artifacts")
    if not isinstance(rows, list) or not rows:
        reasons.append("ARTIFACT_ROWS_INVALID")
        return reasons
    root = artifact_root.resolve(strict=False)
    seen: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            reasons.append("ARTIFACT_ROW_INVALID")
            continue
        value = row.get("path")
        if not isinstance(value, str) or not value or "\\" in value:
            reasons.append("ARTIFACT_PATH_INVALID")
            continue
        pure = PurePosixPath(value)
        if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
            reasons.append(f"ARTIFACT_PATH_INVALID:{value}")
            continue
        relative = pure.as_posix()
        if relative in seen:
            reasons.append(f"ARTIFACT_PATH_DUPLICATE:{relative}")
            continue
        seen.add(relative)
        path = (root / Path(*pure.parts)).resolve(strict=False)
        try:
            path.relative_to(root)
        except ValueError:
            reasons.append(f"ARTIFACT_PATH_INVALID:{relative}")
            continue
        if not path.is_file():
            reasons.append(f"ARTIFACT_MISSING:{relative}")
            continue
        expected_size = row.get("bytes")
        if (
            isinstance(expected_size, bool)
            or not isinstance(expected_size, int)
            or expected_size < 0
            or path.stat().st_size != expected_size
        ):
            reasons.append(f"ARTIFACT_SIZE_MISMATCH:{relative}")
        expected_digest = row.get("sha256")
        try:
            actual_digest = sha256_file(path)
        except OSError:
            reasons.append(f"ARTIFACT_UNREADABLE:{relative}")
            continue
        if actual_digest != expected_digest:
            reasons.append(f"ARTIFACT_DIGEST_MISMATCH:{relative}")
    return reasons


def _capability_reasons(
    provider_capabilities: Mapping[str, Sequence[str]],
    *,
    required_capability: str,
) -> list[str]:
    if not provider_capabilities:
        return ["PROVIDER_CAPABILITIES_MISSING"]
    reasons = []
    for provider in sorted(provider_capabilities):
        values = provider_capabilities[provider]
        if required_capability not in values:
            reasons.append(
                f"PROVIDER_CAPABILITY_MISSING:{provider}:{required_capability}")
    return reasons


def run_campaign_preflight(
    *,
    candidate: Mapping[str, object],
    campaign: Mapping[str, object],
    artifact_root: Path | str,
    artifact_manifest: Mapping[str, object],
    repo_root: Path | str,
    projected_new_bytes: int,
    reserve_bytes: int = DEFAULT_RESERVE_BYTES,
    free_bytes: int | None = None,
    expected_uid: int | None = None,
    provider_capabilities: Mapping[str, Sequence[str]],
    required_capability: str = REQUIRED_CAPABILITY,
) -> dict[str, Any]:
    """Evaluate preflight without creating the campaign output directory."""

    root = Path(repo_root).resolve()
    try:
        validated_candidate = validate_candidate_identity(candidate)
        validated_campaign = validate_campaign_set(
            [campaign], candidate_id=str(validated_candidate["candidateId"]),
            candidate_digest=digest_object(validated_candidate))[0]
    except IdentityError as exc:
        _fail("PREFLIGHT_IDENTITY_INVALID", str(exc))

    output_relative = str(validated_campaign["outputRoot"])
    try:
        output_path = assert_mutation_allowed(output_relative, repo_root=root)
    except ValueError as exc:
        _fail("PREFLIGHT_OUTPUT_INVALID", str(exc))
    reasons: list[str] = []
    if output_path.exists() or output_path.is_symlink():
        reasons.append("OUTPUT_EXISTS")

    invalid_record = output_path.with_name(
        output_path.name + ".invalid-preflight.json")
    if invalid_record.exists() or invalid_record.is_symlink():
        reasons.append("INVALID_PREFLIGHT_RECORD_EXISTS")

    marker = output_path.with_name(output_path.name + ".writer.json")
    writer_reason = _writer_reason(marker)
    if writer_reason:
        reasons.append(writer_reason)

    parent = _existing_ancestor(output_path.parent)
    uid = os.getuid() if expected_uid is None else expected_uid
    try:
        if parent.stat().st_uid != uid:
            reasons.append("OUTPUT_PARENT_OWNER_MISMATCH")
    except OSError:
        reasons.append("OUTPUT_PARENT_UNAVAILABLE")

    reasons.extend(_artifact_reasons(
        candidate_id=str(validated_candidate["candidateId"]),
        candidate_artifact_digest=str(validated_candidate["digests"]["artifact"]),
        artifact_root=Path(artifact_root),
        manifest=artifact_manifest,
    ))
    reasons.extend(_capability_reasons(
        provider_capabilities,
        required_capability=required_capability,
    ))

    projected_valid = (
        not isinstance(projected_new_bytes, bool)
        and isinstance(projected_new_bytes, int)
        and projected_new_bytes >= 0
    )
    reserve_valid = (
        not isinstance(reserve_bytes, bool)
        and isinstance(reserve_bytes, int)
        and reserve_bytes >= 0
    )
    if not projected_valid:
        reasons.append("PROJECTED_BYTES_INVALID")
    if not reserve_valid:
        reasons.append("RESERVE_BYTES_INVALID")
    required_bytes = (
        (projected_new_bytes if projected_valid else 0)
        + (reserve_bytes if reserve_valid else 0)
    )
    if free_bytes is None:
        try:
            available = shutil.disk_usage(parent).free
        except OSError:
            available = -1
            reasons.append("FREE_SPACE_UNAVAILABLE")
    elif isinstance(free_bytes, bool) or not isinstance(free_bytes, int) or free_bytes < 0:
        available = -1
        reasons.append("FREE_BYTES_INVALID")
    else:
        available = free_bytes
    if available >= 0 and available <= required_bytes:
        reasons.append("INSUFFICIENT_FREE_SPACE")

    unique_reasons = sorted(set(reasons))
    passed = not unique_reasons
    return {
        "schema": SCHEMA,
        "candidateId": validated_candidate["candidateId"],
        "campaignId": validated_campaign["campaignId"],
        "campaignKind": validated_campaign["kind"],
        "outputRoot": output_relative,
        "artifactSetDigest": artifact_manifest.get("artifactSetDigest"),
        "projectedNewBytes": projected_new_bytes,
        "reserveBytes": reserve_bytes,
        "requiredBytes": required_bytes,
        "freeBytes": available,
        "requiredCapability": required_capability,
        "providerCount": len(provider_capabilities),
        "checkedAt": _utc_now(),
        "verdict": "PASS" if passed else "INVALID_PREFLIGHT",
        "roleStartAllowed": passed,
        "reasons": unique_reasons,
    }


def write_invalid_preflight_record(
    record: Mapping[str, object],
    *,
    repo_root: Path | str,
) -> Path:
    """Exclusively retain one invalid record beside an unwritten output root."""

    if record.get("verdict") != "INVALID_PREFLIGHT" or record.get("roleStartAllowed") is not False:
        _fail("PREFLIGHT_RECORD_NOT_INVALID")
    output = record.get("outputRoot")
    if not isinstance(output, str):
        _fail("PREFLIGHT_OUTPUT_INVALID")
    root = Path(repo_root).resolve()
    output_path = assert_mutation_allowed(output, repo_root=root)
    if output_path.exists():
        _fail("PREFLIGHT_PARTIAL_OUTPUT_EXISTS", output)
    record_path = output_path.with_name(output_path.name + ".invalid-preflight.json")
    assert_mutation_allowed(record_path, repo_root=root)
    record_path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json.dumps(
        dict(record), indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("utf-8")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    try:
        fd = os.open(str(record_path), flags, 0o644)
    except FileExistsError:
        _fail("PREFLIGHT_RECORD_EXISTS", str(record_path))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
    except Exception:
        try:
            record_path.unlink()
        except OSError:
            pass
        raise
    return record_path


def claim_campaign_writer(
    record: Mapping[str, object],
    *,
    repo_root: Path | str,
    pid: int | None = None,
) -> Path:
    """Atomically claim the sole writer after a passing campaign preflight."""

    if (
        record.get("schema") != SCHEMA
        or record.get("verdict") != "PASS"
        or record.get("roleStartAllowed") is not True
        or record.get("reasons") != []
    ):
        _fail("PREFLIGHT_WRITER_RECORD_NOT_PASS")
    output = record.get("outputRoot")
    candidate_id = record.get("candidateId")
    campaign_id = record.get("campaignId")
    if (
        not isinstance(output, str)
        or not isinstance(candidate_id, str) or not candidate_id
        or not isinstance(campaign_id, str) or not campaign_id
    ):
        _fail("PREFLIGHT_WRITER_RECORD_INVALID")
    owner_pid = os.getpid() if pid is None else pid
    if isinstance(owner_pid, bool) or not isinstance(owner_pid, int) or owner_pid <= 0:
        _fail("PREFLIGHT_WRITER_PID_INVALID")
    root = Path(repo_root).resolve()
    output_path = assert_mutation_allowed(output, repo_root=root)
    if output_path.exists() or output_path.is_symlink():
        _fail("PREFLIGHT_OUTPUT_EXISTS", output)
    invalid_path = output_path.with_name(
        output_path.name + ".invalid-preflight.json")
    if invalid_path.exists() or invalid_path.is_symlink():
        _fail("PREFLIGHT_INVALID_RECORD_EXISTS", str(invalid_path))
    marker = output_path.with_name(output_path.name + ".writer.json")
    assert_mutation_allowed(marker, repo_root=root)
    marker.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema": "ndnsf-di-spec107-writer-claim-v1",
        "candidateId": candidate_id,
        "campaignId": campaign_id,
        "outputRoot": output,
        "pid": owner_pid,
        "state": "ACTIVE",
        "claimedAt": _utc_now(),
    }
    encoded = (json.dumps(
        payload, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("utf-8")
    try:
        fd = os.open(str(marker), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError as exc:
        _fail("PREFLIGHT_WRITER_EXISTS", _writer_reason(marker) or "UNKNOWN")
        raise AssertionError("unreachable") from exc
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
    except Exception:
        try:
            marker.unlink()
        except OSError:
            pass
        raise
    return marker


__all__ = [
    "DEFAULT_RESERVE_BYTES",
    "PreflightError",
    "REQUIRED_CAPABILITY",
    "claim_campaign_writer",
    "run_campaign_preflight",
    "write_invalid_preflight_record",
]
