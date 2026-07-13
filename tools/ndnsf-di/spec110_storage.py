#!/usr/bin/env python3
"""Spec 110 project storage admission, promotion, and cleanup safety."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
from typing import Any, Iterable, Mapping


DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
PROJECTION_FIELDS = ("sif", "models", "exports", "evidence")
PROTECTED_CLASSES = (
    "activeJobs", "identities", "acceptedEvidence", "sealedModels",
    "referencedArtifacts", "currentPriorReleases",
)


class StorageError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise StorageError(code + (f":{detail}" if detail else ""))


def _bytes(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        _fail("STORAGE_NUMBER_INVALID", field)
    return value


def _relative(value: object) -> str:
    path = PurePosixPath(str(value))
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        _fail("CLEANUP_PATH_INVALID", str(value))
    return path.as_posix()


def evaluate_admission(value: Mapping[str, object]) -> dict[str, Any]:
    required = {"projectRoot", "homeBulkPaths", "scratchRoot", "quota", "projected", "reserve"}
    if not isinstance(value, Mapping) or set(value) != required:
        _fail("STORAGE_ADMISSION_FIELDS_INVALID")
    project = str(value["projectRoot"])
    if not project.startswith("/project/") or not project.endswith("/ndnsf-di"):
        _fail("STORAGE_PROJECT_ROOT_INVALID")
    home_bulk = value["homeBulkPaths"]
    if not isinstance(home_bulk, list) or home_bulk:
        _fail("STORAGE_HOME_BULK_FORBIDDEN")
    scratch = str(value["scratchRoot"])
    if not re.fullmatch(r"/tmp/[^/]+/ndnsf-di/[0-9]+", scratch):
        _fail("STORAGE_SCRATCH_ROOT_INVALID")
    quota = value["quota"]
    if not isinstance(quota, Mapping):
        _fail("STORAGE_QUOTA_INVALID")
    source = str(quota.get("source", ""))
    if not source or source.strip().split()[0] == "df":
        _fail("STORAGE_QUOTA_SOURCE_INVALID")
    limit = _bytes(quota.get("limitBytes"), "limitBytes")
    used = _bytes(quota.get("usedBytes"), "usedBytes")
    projected = value["projected"]
    if not isinstance(projected, Mapping) or set(projected) != set(PROJECTION_FIELDS):
        _fail("STORAGE_PROJECTION_INVALID")
    peak = sum(_bytes(projected[field], field) for field in PROJECTION_FIELDS)
    reserve = value["reserve"]
    if not isinstance(reserve, Mapping) or set(reserve) != {"minimumBytes", "fractionBasisPoints"}:
        _fail("STORAGE_RESERVE_INVALID")
    minimum = _bytes(reserve["minimumBytes"], "minimumBytes")
    basis_points = _bytes(reserve["fractionBasisPoints"], "fractionBasisPoints")
    if basis_points > 10000:
        _fail("STORAGE_RESERVE_INVALID", "fractionBasisPoints")
    reserve_bytes = max(minimum, (peak * basis_points + 9999) // 10000)
    available = max(0, limit - used)
    required_bytes = peak + reserve_bytes
    if quota.get("verified") is not True:
        status, reason = "BLOCKED", "QUOTA_NOT_VERIFIED"
    elif available < required_bytes:
        status, reason = "BLOCKED", "QUOTA_RESERVE_INSUFFICIENT"
    else:
        status, reason = "PASS", ""
    return {
        "status": status,
        "reasonCode": reason,
        "projectedPeakBytes": peak,
        "reserveBytes": reserve_bytes,
        "requiredBytes": required_bytes,
        "quotaAvailableBytes": available,
        "quotaSource": source,
    }


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def verify_promotion_tree(staging: Path | str, manifest: Mapping[str, str]) -> dict[str, Any]:
    root = Path(staging)
    if not root.is_dir() or not isinstance(manifest, Mapping) or not manifest:
        _fail("PROMOTION_INPUT_INVALID")
    expected = {_relative(path): digest for path, digest in manifest.items()}
    if any(not isinstance(digest, str) or DIGEST_RE.fullmatch(digest) is None for digest in expected.values()):
        _fail("PROMOTION_MANIFEST_DIGEST_INVALID")
    actual = sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())
    if actual != sorted(expected):
        _fail("PROMOTION_PARTIAL_COPY", f"expected={sorted(expected)},actual={actual}")
    for relative, digest in expected.items():
        if _hash_file(root / relative) != digest:
            _fail("PROMOTION_CHECKSUM_MISMATCH", relative)
    manifest_digest = "sha256:" + hashlib.sha256(
        json.dumps(expected, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return {"status": "PASS", "fileCount": len(expected), "manifestDigest": manifest_digest}


def atomic_promote(
    staging: Path | str,
    target: Path | str,
    manifest: Mapping[str, str],
    *,
    allow_test_root: bool = False,
) -> dict[str, Any]:
    source = Path(staging)
    destination = Path(target)
    if not allow_test_root and not str(destination).startswith("/project/"):
        _fail("PROMOTION_TARGET_NOT_PROJECT")
    if destination.exists():
        _fail("PROMOTION_TARGET_EXISTS")
    report = verify_promotion_tree(source, manifest)
    destination.parent.mkdir(parents=True, exist_ok=True)
    os.replace(source, destination)
    verified = verify_promotion_tree(destination, manifest)
    if verified != report:
        _fail("PROMOTION_POST_RENAME_MISMATCH")
    return {**verified, "target": str(destination), "complete": True}


def plan_cleanup(
    candidates: Iterable[str],
    protected: Mapping[str, Iterable[str]],
    *,
    dry_run: bool = True,
) -> dict[str, Any]:
    if not isinstance(protected, Mapping) or set(protected) != set(PROTECTED_CLASSES):
        _fail("CLEANUP_PROTECTED_FIELDS_INVALID")
    protected_paths = {
        _relative(item)
        for field in PROTECTED_CLASSES
        for item in protected[field]
    }
    deletable, retained = [], []
    for candidate in sorted({_relative(item) for item in candidates}):
        if any(candidate == item or candidate.startswith(item.rstrip("/") + "/") for item in protected_paths):
            retained.append(candidate)
        else:
            deletable.append(candidate)
    return {
        "dryRun": dry_run,
        "deleteCandidates": deletable,
        "protected": retained,
        "executed": False,
    }


__all__ = [
    "StorageError", "atomic_promote", "evaluate_admission", "plan_cleanup",
    "verify_promotion_tree",
]
