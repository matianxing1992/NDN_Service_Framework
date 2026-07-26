#!/usr/bin/env python3
"""Prepare and verify checksum-bound Git archives for the GPU OCI build."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile


SCHEMA = "spec110-oci-source-seal-v1"
OWNER_PROFILES = (
    "core", "sdk", "app", "planner", "adapters/onnx",
    "adapters/qwen", "adapters/llama", "ops", "compat",
)
ARCHIVE_CONFIG = (
    "-c", "filter.lfs.process=",
    "-c", "filter.lfs.smudge=",
    "-c", "filter.lfs.required=false",
)


class SealError(RuntimeError):
    """Fail-closed sealed-context error with a stable reason code."""


def _run(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise SealError(f"SOURCE_SEAL_GIT_FAILED:{repo.name}:{args[0]}")
    return result.stdout.strip()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _body_digest(value: dict) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _safe_member_path(value: str) -> bool:
    path = PurePosixPath(value)
    return not path.is_absolute() and value not in {"", "."} and ".." not in path.parts


def validate_archive(path: Path) -> None:
    """Reject traversal and special-file members before any tar extraction."""
    try:
        with tarfile.open(path, "r:") as archive:
            members = archive.getmembers()
            if not members:
                raise SealError("SOURCE_SEAL_ARCHIVE_EMPTY")
            for member in members:
                if not _safe_member_path(member.name):
                    raise SealError(f"SOURCE_SEAL_ARCHIVE_UNSAFE:{member.name}")
                if member.issym() or member.islnk():
                    if not _safe_member_path(member.linkname):
                        raise SealError(f"SOURCE_SEAL_ARCHIVE_UNSAFE:{member.name}")
                elif not (member.isfile() or member.isdir()):
                    raise SealError(f"SOURCE_SEAL_ARCHIVE_UNSAFE:{member.name}")
    except tarfile.TarError as error:
        raise SealError("SOURCE_SEAL_ARCHIVE_INVALID") from error


def _archive(repo: Path, revision: str, output: Path) -> dict[str, object]:
    try:
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o640)
    except FileExistsError as error:
        raise SealError(f"SOURCE_SEAL_ARCHIVE_EXISTS:{output.name}") from error
    process = None
    try:
        with os.fdopen(descriptor, "wb") as stream:
            process = subprocess.run(
                ["git", "-C", str(repo), *ARCHIVE_CONFIG, "archive", "--format=tar", revision],
                stdout=stream,
                stderr=subprocess.PIPE,
                check=False,
            )
            stream.flush()
            os.fsync(stream.fileno())
        if process.returncode:
            raise SealError(f"SOURCE_SEAL_ARCHIVE_FAILED:{repo.name}")
        validate_archive(output)
        return {"archiveDigest": _sha256(output), "archiveBytes": output.stat().st_size}
    except Exception:
        output.unlink(missing_ok=True)
        raise


def _workspace_record(workspace: Path) -> dict[str, object]:
    revision = _run(workspace, "rev-parse", "HEAD")
    if _run(workspace, "status", "--porcelain", "--untracked-files=no"):
        raise SealError("SOURCE_SEAL_TRACKED_DIRTY:workspace")
    process = subprocess.Popen(
        ["git", "-C", str(workspace), *ARCHIVE_CONFIG, "archive", "--format=tar", revision],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None
    digest = hashlib.sha256()
    count = 0
    for chunk in iter(lambda: process.stdout.read(1024 * 1024), b""):
        digest.update(chunk)
        count += len(chunk)
    stderr = process.stderr.read() if process.stderr is not None else b""
    if process.wait() != 0:
        raise SealError(f"SOURCE_SEAL_WORKSPACE_ARCHIVE_FAILED:{stderr.decode(errors='replace')}")
    return {
        "revision": revision,
        "archiveDigest": "sha256:" + digest.hexdigest(),
        "archiveBytes": count,
    }


def _load_lock(lock_path: Path) -> dict[str, dict[str, str]]:
    value = json.loads(lock_path.read_text(encoding="utf-8"))
    sources = value.get("sourceRepositories")
    if value.get("schemaVersion") != "ndnsf-di-gpu-lock-v1" or not isinstance(sources, dict):
        raise SealError("SOURCE_SEAL_LOCK_INVALID")
    for name, row in sources.items():
        if (
            not isinstance(name, str)
            or not name
            or "/" in name
            or not isinstance(row, dict)
            or set(row) != {"url", "revision"}
            or not isinstance(row["url"], str)
            or not isinstance(row["revision"], str)
            or len(row["revision"]) != 40
            or any(character not in "0123456789abcdef" for character in row["revision"])
        ):
            raise SealError(f"SOURCE_SEAL_LOCK_INVALID:{name}")
    return sources


def _dependency_repo(
    name: str,
    row: dict[str, str],
    dependency_root: Path | None,
    work_root: Path,
) -> Path:
    if dependency_root is not None:
        repo = dependency_root / name
        result = subprocess.run(
            ["git", "-C", str(repo), "cat-file", "-e", row["revision"] + "^{commit}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode:
            raise SealError(f"SOURCE_SEAL_REVISION_MISMATCH:{name}")
        return repo
    repo = work_root / name
    repo.mkdir(parents=True, exist_ok=False)
    subprocess.run(["git", "-C", str(repo), "init", "-q"], check=True)
    result = subprocess.run(
        ["git", "-C", str(repo), "fetch", "--no-tags", "--depth=1", row["url"], row["revision"]],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise SealError(f"SOURCE_SEAL_FETCH_FAILED:{name}")
    fetched = _run(repo, "rev-parse", "FETCH_HEAD")
    if fetched != row["revision"]:
        raise SealError(f"SOURCE_SEAL_REVISION_MISMATCH:{name}")
    return repo


def create(
    workspace: Path,
    lock_path: Path,
    output: Path,
    dependency_root: Path | None,
    work_root: Path,
) -> str:
    if os.path.lexists(output):
        raise SealError("SOURCE_SEAL_OUTPUT_EXISTS")
    if os.path.lexists(work_root):
        raise SealError("SOURCE_SEAL_WORK_ROOT_EXISTS")
    staging = output.with_name(output.name + f".partial.{os.getpid()}")
    if os.path.lexists(staging):
        raise SealError("SOURCE_SEAL_PARTIAL_EXISTS")
    sources = _load_lock(lock_path)
    staging_archives = staging / "archives"
    staging_archives.mkdir(parents=True, mode=0o750)
    work_root.mkdir(parents=True, mode=0o700)
    try:
        dependencies = {}
        for name, row in sorted(sources.items()):
            repo = _dependency_repo(name, row, dependency_root, work_root)
            archive_path = staging_archives / f"{name}.tar"
            archive = _archive(repo, row["revision"], archive_path)
            dependencies[name] = {
                "revision": row["revision"],
                "archivePath": f"archives/{name}.tar",
                **archive,
            }
        body = {
            "schemaVersion": SCHEMA,
            "createdAt": dt.datetime.now(dt.timezone.utc).isoformat(),
            "workspace": _workspace_record(workspace),
            "dependencies": dependencies,
            "lockDigest": _sha256(lock_path),
            "ndnsfDiOwnerProfiles": list(OWNER_PROFILES),
            "modelWeightsIncluded": False,
        }
        body["sealDigest"] = _body_digest(body)
        manifest = staging / "source-seal.json"
        with manifest.open("x", encoding="utf-8") as stream:
            json.dump(body, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(staging, output)
        return str(body["sealDigest"])
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    finally:
        shutil.rmtree(work_root, ignore_errors=True)


def verify(workspace: Path, lock_path: Path, output: Path) -> str:
    sources = _load_lock(lock_path)
    manifest = output / "source-seal.json"
    value = json.loads(manifest.read_text(encoding="utf-8"))
    actual = dict(value)
    seal_digest = actual.pop("sealDigest", None)
    if value.get("schemaVersion") != SCHEMA or _body_digest(actual) != seal_digest:
        raise SealError("SOURCE_SEAL_MANIFEST_TAMPERED")
    if value.get("lockDigest") != _sha256(lock_path):
        raise SealError("SOURCE_SEAL_LOCK_MISMATCH")
    if value.get("workspace") != _workspace_record(workspace):
        raise SealError("SOURCE_SEAL_WORKSPACE_MISMATCH")
    dependencies = value.get("dependencies")
    if not isinstance(dependencies, dict) or set(dependencies) != set(sources):
        raise SealError("SOURCE_SEAL_DEPENDENCIES_MISMATCH")
    for name, row in sorted(sources.items()):
        measured = dependencies[name]
        if not isinstance(measured, dict):
            raise SealError(f"SOURCE_SEAL_DEPENDENCY_INVALID:{name}")
        expected_path = f"archives/{name}.tar"
        if measured.get("revision") != row["revision"] or measured.get("archivePath") != expected_path:
            raise SealError(f"SOURCE_SEAL_REVISION_MISMATCH:{name}")
        archive = output / expected_path
        validate_archive(archive)
        if (
            measured.get("archiveDigest") != _sha256(archive)
            or measured.get("archiveBytes") != archive.stat().st_size
        ):
            raise SealError(f"SOURCE_SEAL_ARCHIVE_MISMATCH:{name}")
    return str(seal_digest)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("--workspace", required=True)
    parser.add_argument("--lock", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--dependency-root")
    parser.add_argument("--work-root")
    args = parser.parse_args()
    workspace = Path(args.workspace).resolve()
    lock_path = Path(args.lock).resolve()
    output = Path(args.output).resolve()
    if args.action == "create":
        dependency_root = Path(args.dependency_root).resolve() if args.dependency_root else None
        work_root = (
            Path(args.work_root).resolve()
            if args.work_root
            else output.with_name(output.name + f".fetch.{os.getpid()}")
        )
        print(create(workspace, lock_path, output, dependency_root, work_root))
    else:
        print(verify(workspace, lock_path, output))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError, KeyError, SealError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(4)
