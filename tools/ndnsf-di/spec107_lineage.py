#!/usr/bin/env python3
"""Read-only Spec 107 lineage verification and Spec 105 mutation guard."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
from typing import Any, Mapping


SCHEMA = "ndnsf-di-lineage-lock-v1"
PREDECESSOR_SPEC = "specs/105-ndnsf-di-deployment-readiness"
PREDECESSOR_RELEASE_ID = "spec105-local-minindn-candidate-r2"
EXPECTED_CLASSIFICATIONS = frozenset({
    "task-closure",
    "release-decision",
    "performance-negative-evidence",
    "recovery-negative-evidence",
})
SHA256_RE = re.compile(r"^sha256:([0-9a-f]{64})$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


class LineageError(ValueError):
    """Stable fail-closed lineage validation error."""


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _fail(code: str, detail: str = "") -> None:
    suffix = f":{detail}" if detail else ""
    raise LineageError(code + suffix)


def _load_json_object(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        _fail("LINEAGE_LOCK_MISSING", str(path))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _fail("LINEAGE_LOCK_INVALID", str(exc))
    if not isinstance(payload, dict):
        _fail("LINEAGE_LOCK_INVALID", "root-not-object")
    return payload


def _safe_repo_relative(value: object, *, repo_root: Path) -> tuple[str, Path]:
    if not isinstance(value, str) or not value or "\\" in value:
        _fail("LINEAGE_PATH_INVALID", repr(value))
    pure = PurePosixPath(value)
    if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
        _fail("LINEAGE_PATH_INVALID", value)
    candidate = (repo_root / Path(*pure.parts)).resolve(strict=False)
    try:
        candidate.relative_to(repo_root)
    except ValueError:
        _fail("LINEAGE_PATH_INVALID", value)
    return pure.as_posix(), candidate


def _validate_lock_fields(payload: Mapping[str, Any]) -> None:
    if payload.get("schema") != SCHEMA:
        _fail("LINEAGE_SCHEMA_INVALID", repr(payload.get("schema")))
    if payload.get("predecessorSpec") != PREDECESSOR_SPEC:
        _fail("LINEAGE_PREDECESSOR_INVALID", repr(payload.get("predecessorSpec")))
    if payload.get("predecessorReleaseId") != PREDECESSOR_RELEASE_ID:
        _fail("LINEAGE_RELEASE_ID_INVALID", repr(payload.get("predecessorReleaseId")))
    if payload.get("predecessorMiniNdnVerdict") != "BLOCK":
        _fail("LINEAGE_MININDN_VERDICT_INVALID")
    if payload.get("predecessorPhysicalVerdict") != "DEFERRED":
        _fail("LINEAGE_PHYSICAL_VERDICT_INVALID")
    commit = payload.get("frozenCommit")
    if not isinstance(commit, str) or COMMIT_RE.fullmatch(commit) is None:
        _fail("LINEAGE_FROZEN_COMMIT_INVALID", repr(commit))


def load_lineage_lock(
    lock_path: Path | str,
    *,
    repo_root: Path | str | None = None,
) -> dict[str, Any]:
    """Load and structurally validate a lineage lock without mutating files."""

    root = Path(repo_root or default_repo_root()).resolve()
    payload = _load_json_object(Path(lock_path))
    _validate_lock_fields(payload)
    rows = payload.get("files")
    if not isinstance(rows, list):
        _fail("LINEAGE_FILES_INVALID", "not-array")

    seen_paths: set[str] = set()
    classifications: set[str] = set()
    normalized_rows: list[dict[str, str]] = []
    predecessor_root = (root / PREDECESSOR_SPEC).resolve(strict=False)
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            _fail("LINEAGE_FILE_INVALID", str(index))
        relative, resolved = _safe_repo_relative(row.get("path"), repo_root=root)
        if relative in seen_paths:
            _fail("LINEAGE_PATH_DUPLICATE", relative)
        seen_paths.add(relative)
        try:
            resolved.relative_to(predecessor_root)
        except ValueError:
            _fail("LINEAGE_PATH_INVALID", relative)
        classification = row.get("classification")
        if not isinstance(classification, str):
            _fail("LINEAGE_CLASSIFICATION_INVALID", str(index))
        classifications.add(classification)
        digest = row.get("sha256")
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            _fail("LINEAGE_DIGEST_INVALID", relative)
        normalized_rows.append({
            "classification": classification,
            "path": relative,
            "sha256": digest,
        })

    if classifications != EXPECTED_CLASSIFICATIONS or len(normalized_rows) != 4:
        _fail(
            "LINEAGE_CLASSIFICATIONS_INVALID",
            ",".join(sorted(classifications)),
        )
    normalized = dict(payload)
    normalized["files"] = normalized_rows
    return normalized


def sha256_file(path: Path, *, chunk_bytes: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _verify_frozen_commit(root: Path, commit: str) -> None:
    exists = subprocess.run(
        ["git", "cat-file", "-e", f"{commit}^{{commit}}"],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if exists.returncode != 0:
        _fail("LINEAGE_FROZEN_COMMIT_MISSING", commit)
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", commit, "HEAD"],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if ancestor.returncode != 0:
        _fail("LINEAGE_FROZEN_COMMIT_NOT_ANCESTOR", commit)


def verify_lineage_lock(
    lock_path: Path | str,
    *,
    repo_root: Path | str | None = None,
    verify_commit: bool = True,
) -> dict[str, Any]:
    """Verify all locked files and return a deterministic in-memory report."""

    root = Path(repo_root or default_repo_root()).resolve()
    payload = load_lineage_lock(lock_path, repo_root=root)
    commit = str(payload["frozenCommit"])
    if verify_commit:
        _verify_frozen_commit(root, commit)

    verified = []
    for row in payload["files"]:
        relative = row["path"]
        path = (root / relative).resolve(strict=False)
        if not path.is_file():
            _fail("LINEAGE_FILE_MISSING", relative)
        actual = sha256_file(path)
        if actual != row["sha256"]:
            _fail(
                "LINEAGE_DIGEST_MISMATCH",
                f"{relative}:expected={row['sha256']}:actual={actual}",
            )
        verified.append({
            "classification": row["classification"],
            "path": relative,
            "sha256": actual,
        })
    return {
        "schema": "ndnsf-di-lineage-verification-v1",
        "status": "PASS",
        "frozenCommit": commit,
        "predecessorReleaseId": payload["predecessorReleaseId"],
        "predecessorMiniNdnVerdict": payload["predecessorMiniNdnVerdict"],
        "predecessorPhysicalVerdict": payload["predecessorPhysicalVerdict"],
        "verifiedFileCount": len(verified),
        "verifiedIdentifierCount": len(verified) + 1,
        "files": verified,
    }


def _relative_if_inside(path: Path, root: Path) -> PurePosixPath | None:
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    return PurePosixPath(relative.as_posix())


def _is_frozen_relative(relative: PurePosixPath) -> bool:
    parts = relative.parts
    if len(parts) >= 2 and parts[0] == "specs" and parts[1].startswith("105-"):
        return True
    if len(parts) >= 2 and parts[0] == "results" and parts[1].startswith("spec105-"):
        return True
    return False


def assert_mutation_allowed(
    target: Path | str,
    *,
    repo_root: Path | str | None = None,
) -> Path:
    """Return a resolved target or reject any lexical/resolved Spec 105 path."""

    root = Path(repo_root or default_repo_root()).resolve()
    raw = Path(target)
    lexical = raw if raw.is_absolute() else root / raw
    lexical = Path(str(lexical).replace("\\", "/"))
    lexical_relative = _relative_if_inside(lexical, root)
    resolved = lexical.resolve(strict=False)
    resolved_relative = _relative_if_inside(resolved, root)
    if (
        lexical_relative is None
        or resolved_relative is None
        or _is_frozen_relative(lexical_relative)
        or _is_frozen_relative(resolved_relative)
    ):
        _fail("SPEC105_MUTATION_DENIED", str(target))
    return resolved


__all__ = [
    "LineageError",
    "assert_mutation_allowed",
    "default_repo_root",
    "load_lineage_lock",
    "sha256_file",
    "verify_lineage_lock",
]
