"""Freeze the small, explicit YOLO harness without copying runtime/model data.

This is content integrity, not source approval or runtime qualification. T007
must review the exact inventory, and launch gates must verify the bundle again.
Owner-writable parent directories are not a security boundary against that owner;
workers additionally mount this checked directory read-only.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat

from .yolo_profile import ClosureError, HASH, _file_identity, _object, _operator_path


# Explicit dependency closure: no recursive copying of the repository or models.
# Missing future application/collector files keep a production bundle incomplete.
REQUIRED_HARNESS_FILES = frozenset({
    "runtime/__init__.py", "runtime/baseline.py", "runtime/identities.py",
    "runtime/worker.py", "runtime/yolo_worker.py", "runtime/yolo_profile.py",
    "runtime/yolo_submission.py", "runtime/yolo_bundle.py", "runtime/yolo_result.py",
    "apps/yolo.py", "jobs/yolo/submit.py", "jobs/yolo/run.sbatch",
    "schemas/tiger-yolo-v1.schema.json", "requirements-operator.txt",
})
MANIFEST = "harness-manifest.json"
MAX_FILE_BYTES = 4 * 1024 * 1024
MAX_TOTAL_BYTES = 16 * 1024 * 1024
PRIVATE_PEM = re.compile(rb"^-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----\r?$", re.M)


def _bytes(path):
    try:
        fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_FILE_BYTES:
                raise ClosureError("HARNESS_FILE_SIZE_OR_TYPE")
            payload = stream.read(MAX_FILE_BYTES + 1)
        if len(payload) > MAX_FILE_BYTES:
            raise ClosureError("HARNESS_FILE_SIZE_OR_TYPE")
        return payload
    except OSError as exc:
        raise ClosureError("HARNESS_FILE_UNAVAILABLE") from exc


def _load(manifest, expected, source_root=None):
    if not isinstance(expected, str) or not HASH.fullmatch(expected):
        raise ClosureError("HARNESS_MANIFEST_DIGEST")
    manifest = Path(_operator_path(str(manifest), Path.cwd(), local=True))
    source_root = (manifest.parent if source_root is None else
                   Path(_operator_path(str(source_root), Path.cwd(), local=True)))
    raw = _bytes(manifest)
    if "sha256:" + hashlib.sha256(raw).hexdigest() != expected:
        raise ClosureError("HARNESS_MANIFEST_DIGEST")
    try:
        value = json.loads(raw, object_pairs_hook=_object)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ClosureError("HARNESS_MANIFEST_DOCUMENT") from exc
    if (not isinstance(value, dict) or set(value) != {"schema", "files"}
            or value["schema"] != "tiger-yolo-harness-v1"
            or not isinstance(value["files"], dict)
            or set(value["files"]) != REQUIRED_HARNESS_FILES):
        raise ClosureError("HARNESS_INVENTORY")
    total = 0
    for row in value["files"].values():
        if (not isinstance(row, dict) or set(row) != {"bytes", "sha256"}
                or type(row["bytes"]) is not int or not 0 <= row["bytes"] <= MAX_FILE_BYTES
                or not isinstance(row["sha256"], str) or not HASH.fullmatch(row["sha256"])):
            raise ClosureError("HARNESS_FILE_RECORD")
        total += row["bytes"]
    if total > MAX_TOTAL_BYTES:
        raise ClosureError("HARNESS_TOTAL_BYTES")
    payloads = {}
    for name, row in sorted(value["files"].items()):
        _file_identity(source_root, name, dict(row, path=name))
        raw_file = _bytes(source_root / name)
        if (len(raw_file) != row["bytes"]
                or "sha256:" + hashlib.sha256(raw_file).hexdigest() != row["sha256"]):
            raise ClosureError("HARNESS_CHANGED_DURING_CHECK:" + name)
        try:
            raw_file.decode("utf-8")
        except UnicodeError as exc:
            raise ClosureError("HARNESS_NOT_TEXT:" + name) from exc
        if b"\x00" in raw_file or PRIVATE_PEM.search(raw_file):
            raise ClosureError("HARNESS_FORBIDDEN_CONTENT:" + name)
        payloads[name] = raw_file
    return raw, payloads


def verify_harness(root: Path, *, expected_manifest_sha256: str) -> dict:
    """Check exact inventory, copied bytes and read-only filesystem modes."""
    root = Path(_operator_path(str(root), Path.cwd(), local=True))
    raw, payloads = _load(root / MANIFEST, expected_manifest_sha256)
    expected_files = set(payloads) | {MANIFEST}
    expected_dirs = {"."}
    for name in expected_files:
        expected_dirs.update(str(p) for p in Path(name).parents)
    def sealed(path):
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or info.st_mode & 0o222:
            raise ClosureError("HARNESS_NOT_SEALED")
        return info.st_mode

    actual_files, actual_dirs = set(), {"."}
    # Traverse only known directories. Reject an unexpected subtree at its
    # first entry, rather than recursively scanning arbitrary injected data.
    for name in sorted(expected_dirs):
        directory = root / name
        if not stat.S_ISDIR(sealed(directory)):
            raise ClosureError("HARNESS_DIRECTORY_TYPE")
        with os.scandir(directory) as entries:
            for entry in entries:
                relative = str(Path(entry.path).relative_to(root))
                if relative not in expected_files | expected_dirs:
                    raise ClosureError("HARNESS_EXTRA_CONTENT")
                mode = sealed(Path(entry.path))
                if stat.S_ISDIR(mode):
                    actual_dirs.add(relative)
                elif stat.S_ISREG(mode):
                    actual_files.add(relative)
                else:
                    raise ClosureError("HARNESS_FILE_TYPE")
    if actual_files != expected_files or actual_dirs != expected_dirs:
        raise ClosureError("HARNESS_EXTRA_CONTENT")
    return {"manifestSha256": expected_manifest_sha256, "files": len(payloads),
            "bytes": len(raw) + sum(map(len, payloads.values())),
            "integrity": "VERIFIED", "qualification": "NOT_EVALUATED"}


def freeze_harness(manifest: Path, destination: Path, *, expected_manifest_sha256: str,
                   source_root: Path | None = None) -> dict:
    """Copy verified small source bytes, not hardlinks to a mutable worktree.

    Validate every file before creating a destination. Destination must be new
    under an existing parent; partial failed writes are retained and never reused.
    The manifest is written last, then the tree is made read-only and rechecked.
    source_root lets generated manifests live outside the working tree; it
    changes physical lookup only, never expected file identities. No subprocess,
    SIF, model, secret provisioning or remote operation occurs.
    """
    raw, payloads = _load(manifest, expected_manifest_sha256, source_root=source_root)
    destination = Path(_operator_path(str(destination), Path.cwd(), local=True))
    if not destination.parent.is_dir():
        raise ClosureError("HARNESS_DESTINATION_PARENT")
    destination.mkdir(mode=0o700, exist_ok=False)
    for name, payload in list(payloads.items()) + [(MANIFEST, raw)]:
        path = destination / name
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        path.chmod(0o444)
    directories = [p for p in destination.rglob("*") if p.is_dir()] + [destination]
    for directory in sorted(directories, key=lambda p: len(p.parts), reverse=True):
        directory.chmod(0o555)
        fd = os.open(str(directory), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    return verify_harness(destination, expected_manifest_sha256=expected_manifest_sha256)
