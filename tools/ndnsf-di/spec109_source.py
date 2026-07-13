#!/usr/bin/env python3
"""Reproducible Spec 109 source snapshots and campaign identities."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
from typing import Any, Mapping


SHA256_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
CAMPAIGN_RE = re.compile(r"^spec109(?:-[0-9a-f]{12}){4}$")


class SourceSnapshotError(ValueError):
    """Stable fail-closed snapshot error."""


def _fail(code: str, detail: str = "") -> None:
    raise SourceSnapshotError(code + (f":{detail}" if detail else ""))


def canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_object(value: object) -> str:
    return digest_bytes(canonical_json(value))


def _git(root: Path, *args: str) -> bytes:
    try:
        result = subprocess.run(
            ["git", *args], cwd=root, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        _fail("SOURCE_GIT_UNAVAILABLE", str(exc))
    if result.returncode:
        _fail("SOURCE_GIT_FAILED", result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _untracked(root: Path) -> tuple[list[dict[str, object]], list[tuple[str, Path]]]:
    raw = _git(root, "ls-files", "--others", "--exclude-standard", "-z")
    names = sorted(x.decode("utf-8", "surrogateescape") for x in raw.split(b"\0") if x)
    rows: list[dict[str, object]] = []
    files: list[tuple[str, Path]] = []
    for name in names:
        pure = PurePosixPath(name)
        if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
            _fail("SOURCE_UNTRACKED_PATH_INVALID", name)
        path = root / Path(*pure.parts)
        if not path.is_file() or path.is_symlink():
            _fail("SOURCE_UNTRACKED_KIND_UNSUPPORTED", name)
        stat = path.stat()
        rows.append({
            "path": pure.as_posix(), "size": stat.st_size,
            "mode": oct(stat.st_mode & 0o777), "digest": _sha256_file(path),
        })
        files.append((pure.as_posix(), path))
    return rows, files


def _deterministic_tar(files: list[tuple[str, Path]]) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for name, path in files:
            data = path.read_bytes()
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            info.mode = path.stat().st_mode & 0o777
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


def capture_source_snapshot(
    repo_root: Path | str,
    *,
    captured_at: str | None = None,
    untracked_archive: Path | str | None = None,
) -> dict[str, Any]:
    """Capture clean HEAD or seal tracked/untracked worktree bytes."""

    root = Path(repo_root).resolve()
    head = _git(root, "rev-parse", "HEAD").decode().strip()
    if COMMIT_RE.fullmatch(head) is None:
        _fail("SOURCE_HEAD_INVALID", head)
    tree = digest_bytes(_git(root, "ls-tree", "-r", "--full-tree", "HEAD"))
    status = _git(root, "status", "--porcelain=v1", "-z")
    timestamp = captured_at or datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    if not status:
        value: dict[str, Any] = {
            "headCommit": head,
            "treeDigest": tree,
            "capturedAt": timestamp,
            "worktreeState": "CLEAN",
            "binaryDiffDigest": None,
            "untrackedManifestDigest": None,
            "untrackedArchiveDigest": None,
            "includedPaths": ["tracked-files@HEAD"],
            "excludedPaths": ["build/", "results/", "model-weights/", "credentials/"],
        }
    else:
        if untracked_archive is None:
            _fail("SOURCE_DIRTY_ARCHIVE_REQUIRED")
        diff = _git(root, "diff", "--binary", "HEAD", "--")
        rows, files = _untracked(root)
        archive_bytes = _deterministic_tar(files)
        archive_path = Path(untracked_archive)
        archive_path.parent.mkdir(parents=True, exist_ok=True)
        archive_path.write_bytes(archive_bytes)
        value = {
            "headCommit": head,
            "treeDigest": tree,
            "capturedAt": timestamp,
            "worktreeState": "SEALED_DIRTY",
            "binaryDiffDigest": digest_bytes(diff),
            "untrackedManifestDigest": digest_object(rows),
            "untrackedArchiveDigest": digest_bytes(archive_bytes),
            "includedPaths": ["tracked-files@HEAD", "binary-diff@HEAD", "untracked-manifest"],
            "excludedPaths": ["build/", "results/", "model-weights/", "credentials/"],
        }
    value["snapshotDigest"] = digest_object(value)
    return value


def validate_source_snapshot(value: Mapping[str, object]) -> dict[str, Any]:
    fields = {
        "headCommit", "treeDigest", "capturedAt", "worktreeState",
        "binaryDiffDigest", "untrackedManifestDigest", "untrackedArchiveDigest",
        "includedPaths", "excludedPaths", "snapshotDigest",
    }
    if set(value) != fields:
        _fail("SOURCE_FIELDS_INVALID")
    if not isinstance(value.get("headCommit"), str) or COMMIT_RE.fullmatch(str(value["headCommit"])) is None:
        _fail("SOURCE_HEAD_INVALID")
    for field in ("treeDigest", "snapshotDigest"):
        if not isinstance(value.get(field), str) or SHA256_RE.fullmatch(str(value[field])) is None:
            _fail("SOURCE_DIGEST_INVALID", field)
    state = value.get("worktreeState")
    optional = ("binaryDiffDigest", "untrackedManifestDigest", "untrackedArchiveDigest")
    if state == "CLEAN":
        if any(value.get(field) is not None for field in optional):
            _fail("SOURCE_CLEAN_SEAL_INVALID")
    elif state == "SEALED_DIRTY":
        for field in optional:
            if not isinstance(value.get(field), str) or SHA256_RE.fullmatch(str(value[field])) is None:
                _fail("SOURCE_DIRTY_SEAL_INVALID", field)
    else:
        _fail("SOURCE_STATE_INVALID")
    without = dict(value)
    actual = str(without.pop("snapshotDigest"))
    if digest_object(without) != actual:
        _fail("SOURCE_SNAPSHOT_DIGEST_MISMATCH")
    return dict(value)


def campaign_id(
    *, source_digest: str, predecessor_digest: str,
    deployment_digest: str, matrix_digest: str,
) -> str:
    values = (source_digest, predecessor_digest, deployment_digest, matrix_digest)
    if any(not isinstance(value, str) or SHA256_RE.fullmatch(value) is None for value in values):
        _fail("CAMPAIGN_BINDING_DIGEST_INVALID")
    result = "spec109-" + "-".join(value.split(":", 1)[1][:12] for value in values)
    if CAMPAIGN_RE.fullmatch(result) is None:
        _fail("CAMPAIGN_ID_INVALID")
    return result


__all__ = [
    "SourceSnapshotError", "campaign_id", "capture_source_snapshot",
    "digest_bytes", "digest_object", "validate_source_snapshot",
]
