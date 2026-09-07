"""YOLO candidate integrity, independent of runtime qualification and execution.

The operator profile and workload-specific receipt validators must consume
this layer. A verified digest is never permission to build, upload, or submit.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat


REQUIRED_FILES = {
    "inputs": {"sourceLock", "sourceSeal", "buildDefinition", "baseSif"},
    "runtime": {"sif", "nativeManifest", "libraryLock"},
    "dispatch": {"effectiveProfile", "harnessManifest", "modelManifest", "oracle",
                 "fixture", "trustPolicy", "validationContract"},
}
HASH = re.compile(r"sha256:[0-9a-f]{64}\Z")


class ClosureError(ValueError):
    """A candidate cannot be reproduced from the declared inputs."""


def _object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ClosureError("DUPLICATE_JSON_KEY")
        value[key] = item
    return value


def _read_plane(path):
    try:
        fd = os.open(str(path), os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise ClosureError("PLANE_FILE_TYPE")
            content = stream.read(4 * 1024 * 1024 + 1)
        if len(content) > 4 * 1024 * 1024:
            raise ClosureError("PLANE_TOO_LARGE")
        value = json.loads(content, object_pairs_hook=_object)
        # Reject non-finite numbers, including deeply nested values.
        json.dumps(value, allow_nan=False)
        return value
    except (OSError, UnicodeError, ValueError, RecursionError) as exc:
        if isinstance(exc, ClosureError):
            raise
        raise ClosureError("PLANE_DOCUMENT") from exc


def _file_identity(root, name, row):
    if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.:/-]{0,200}", name)
            or not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}):
        raise ClosureError("FILE_RECORD")
    if (type(row["bytes"]) is not int or row["bytes"] < 0
            or not isinstance(row["sha256"], str) or not HASH.fullmatch(row["sha256"])):
        raise ClosureError("FILE_METADATA:" + name)
    relative = row["path"]
    if (not isinstance(relative, str) or not relative
            or any(c in relative for c in "\x00\n\r\\")
            or PurePosixPath(relative).is_absolute()
            or any(part in ("", ".", "..") for part in relative.split("/"))):
        raise ClosureError("FILE_PATH:" + name)
    file = root
    for part in PurePosixPath(relative).parts:
        file = file / part
        if file.is_symlink():
            raise ClosureError("FILE_SYMLINK:" + name)
    try:
        file.resolve().relative_to(root)
    except ValueError as exc:
        raise ClosureError("FILE_PATH:" + name) from exc
    try:
        # NONBLOCK prevents special files from hanging a preflight. NOFOLLOW
        # rejects a replaced final symlink; workers must recheck before use.
        fd = os.open(str(file), os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat.S_ISREG(before.st_mode) or before.st_size != row["bytes"]:
                raise ClosureError("FILE_SIZE_OR_TYPE:" + name)
            h = hashlib.sha256()
            remaining = before.st_size
            while remaining:
                chunk = stream.read(min(remaining, 4 * 1024 * 1024))
                if not chunk:
                    raise ClosureError("FILE_CHANGED_DURING_CHECK:" + name)
                h.update(chunk)
                remaining -= len(chunk)
            if stream.read(1):
                raise ClosureError("FILE_CHANGED_DURING_CHECK:" + name)
            after = os.fstat(stream.fileno())
        current = file.stat()
        keys = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        if any(getattr(before, key) != getattr(other, key)
               for other in (after, current) for key in keys):
            raise ClosureError("FILE_CHANGED_DURING_CHECK:" + name)
    except OSError as exc:
        raise ClosureError("FILE_UNAVAILABLE:" + name) from exc
    observed = "sha256:" + h.hexdigest()
    if observed != row["sha256"]:
        raise ClosureError("FILE_DIGEST:" + name)
    return {"sha256": observed, "bytes": row["bytes"]}


def check_plane(path: Path, *, expected_stage: str, parent_id=None) -> dict:
    """Verify one content plane without requiring artifacts from a later stage."""
    path = Path(path).resolve()
    value = _read_plane(path)
    if (not isinstance(value, dict)
            or set(value) != {"schema", "stage", "parentId", "files", "parameters"}):
        raise ClosureError("PLANE_FIELDS")
    if not isinstance(expected_stage, str) or expected_stage not in REQUIRED_FILES:
        raise ClosureError("PLANE_STAGE")
    if value["schema"] != "tiger-yolo-plane-v1" or value["stage"] != expected_stage:
        raise ClosureError("PLANE_STAGE")
    if ((expected_stage == "inputs" and parent_id is not None)
            or (expected_stage != "inputs" and
                (not isinstance(parent_id, str) or not HASH.fullmatch(parent_id)))
            or value["parentId"] != parent_id):
        raise ClosureError("PLANE_PARENT")
    if not isinstance(value["parameters"], dict):
        raise ClosureError("PLANE_PARAMETERS")
    if (not isinstance(value["files"], dict) or len(value["files"]) > 4096
            or not REQUIRED_FILES[expected_stage].issubset(value["files"])):
        raise ClosureError("PLANE_INVENTORY")
    identities = {}
    for name, row in value["files"].items():
        identities[name] = _file_identity(path.parent, name, row)
    basis = {"schema": value["schema"], "stage": expected_stage,
             "parentId": parent_id, "files": identities,
             "parameters": value["parameters"]}
    encoded = json.dumps(basis, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    return {"id": "sha256:" + hashlib.sha256(encoded).hexdigest(),
            "stage": expected_stage, "integrity": "VERIFIED",
            "qualification": "NOT_EVALUATED"}


def check_chain(paths: dict, *, through: str) -> dict:
    """Recompute every ancestor; never accept a caller's remembered parent ID.

    This is the content-integrity portion only. Workload validators must still
    establish transitive inventory completeness, safe effective configuration,
    and genuine execution evidence before a launch can be authorized.
    """
    order = tuple(REQUIRED_FILES)
    if through not in order or not isinstance(paths, dict) or set(paths) - set(order):
        raise ClosureError("CHAIN_STAGES")
    needed = order[:order.index(through) + 1]
    if not set(needed).issubset(paths):
        raise ClosureError("CHAIN_STAGES")
    identities = {}
    parent_id = None
    for stage in needed:
        checked = check_plane(paths[stage], expected_stage=stage, parent_id=parent_id)
        parent_id = checked["id"]
        identities[stage] = parent_id
    return {"stage": through, "identities": identities,
            "integrity": "VERIFIED", "qualification": "NOT_EVALUATED"}
