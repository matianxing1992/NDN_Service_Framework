#!/usr/bin/env python3
"""Create and verify deterministic, layer-owned source seals."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tarfile
import tempfile
from typing import Iterable


SCHEMA = "ndnsf-di-layer-seal-v1"
NORMALIZED_CREATED_AT = "1970-01-01T00:00:00+00:00"


class SealError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def body_digest(value: dict[str, object]) -> str:
    body = dict(value)
    body.pop("sealDigest", None)
    body.pop("createdAt", None)
    encoded = json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def validate_archive(path: Path) -> None:
    with tarfile.open(path, "r:") as archive:
        for member in archive.getmembers():
            name = PurePosixPath(member.name)
            link = PurePosixPath(member.linkname)
            if name.is_absolute() or ".." in name.parts:
                raise SealError(f"LAYER_SEAL_ARCHIVE_UNSAFE:{path.name}")
            if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                raise SealError(f"LAYER_SEAL_ARCHIVE_UNSAFE:{path.name}")
            if (member.issym() or member.islnk()) and (
                link.is_absolute() or ".." in link.parts
            ):
                raise SealError(f"LAYER_SEAL_ARCHIVE_UNSAFE:{path.name}")


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise SealError(f"LAYER_SEAL_GIT_FAILED:{repo.name}:{args[0]}")
    return result.stdout.rstrip("\n")


def _excluded(path: str, patterns: Iterable[str]) -> bool:
    posix = PurePosixPath(path)
    for pattern in patterns:
        if fnmatch.fnmatch(path, pattern) or fnmatch.fnmatch(posix.name, pattern):
            return True
        if "/" not in pattern and pattern in posix.parts:
            return True
    return False


def _included(path: str, roots: list[str] | None) -> bool:
    if not roots:
        return True
    return any(path == root or path.startswith(root.rstrip("/") + "/") for root in roots)


def snapshot_git_tree(
    repo: Path,
    output: Path,
    excludes: list[str],
    includes: list[str] | None = None,
) -> dict[str, object]:
    tracked = [
        line for line in git(repo, "ls-files", "-z").split("\0")
        if line and _included(line, includes) and not _excluded(line, excludes)
    ]
    untracked = [
        line
        for line in git(
            repo, "ls-files", "--others", "--exclude-standard", "-z"
        ).split("\0")
        if line and _included(line, includes) and not _excluded(line, excludes)
    ]
    selected = sorted(set(tracked) | set(untracked))
    if not selected:
        raise SealError(f"LAYER_SEAL_EMPTY_WORKSPACE:{repo.name}")
    dirty_candidates = set(untracked)
    for arguments in (
        ("diff", "--name-only", "-z"),
        ("diff", "--cached", "--name-only", "-z"),
    ):
        dirty_candidates.update(
            line for line in git(repo, *arguments).split("\0") if line
        )
    dirty = {
        path for path in dirty_candidates
        if _included(path, includes) and not _excluded(path, excludes)
    }
    with tarfile.open(output, "w", format=tarfile.PAX_FORMAT) as archive:
        for relative in selected:
            source = repo / relative
            if not source.exists() and not source.is_symlink():
                raise SealError(f"LAYER_SEAL_TRACKED_PATH_MISSING:{repo.name}:{relative}")
            info = archive.gettarinfo(str(source), arcname=relative)
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = 0
            info.mode = stat.S_IMODE(info.mode)
            if info.isfile():
                with source.open("rb") as stream:
                    archive.addfile(info, stream)
            else:
                archive.addfile(info)
    validate_archive(output)
    return {
        "baseRevision": git(repo, "rev-parse", "HEAD"),
        "dirty": bool(dirty),
        "dirtyPaths": sorted(dirty),
        "trackedPathCount": len(tracked),
        "untrackedPathCount": len(untracked),
        "archivePath": f"archives/{output.name}",
        "archiveBytes": output.stat().st_size,
        "archiveDigest": sha256(output),
    }


def create_ndn(lock: Path, legacy: Path, output: Path) -> str:
    value = json.loads(lock.read_text())
    sources = value.get("sourceRepositories", {})
    legacy_value = json.loads((legacy / "source-seal.json").read_text())
    staging = output.with_name(output.name + f".partial.{os.getpid()}")
    if output.exists() or staging.exists():
        raise SealError("LAYER_SEAL_OUTPUT_EXISTS")
    (staging / "archives").mkdir(parents=True)
    records: dict[str, object] = {}
    try:
        for name, row in sorted(sources.items()):
            legacy_row = legacy_value.get("dependencies", {}).get(name, {})
            if legacy_row.get("revision") != row.get("revision"):
                raise SealError(f"LAYER_SEAL_LEGACY_REVISION_MISMATCH:{name}")
            source = legacy / str(legacy_row.get("archivePath", ""))
            target = staging / "archives" / f"{name}.tar"
            shutil.copyfile(source, target)
            validate_archive(target)
            if sha256(target) != legacy_row.get("archiveDigest"):
                raise SealError(f"LAYER_SEAL_LEGACY_ARCHIVE_MISMATCH:{name}")
            records[name] = {
                "revision": row["revision"],
                "archivePath": f"archives/{name}.tar",
                "archiveBytes": target.stat().st_size,
                "archiveDigest": sha256(target),
            }
        manifest: dict[str, object] = {
            "schemaVersion": SCHEMA,
            "layer": "ndn-foundation",
            "createdAt": NORMALIZED_CREATED_AT,
            "lockDigest": sha256(lock),
            "sources": records,
        }
        manifest["sealDigest"] = body_digest(manifest)
        (staging / "seal.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )
        os.replace(staging, output)
        return str(manifest["sealDigest"])
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def create_app(
    lock: Path,
    workspace: Path,
    ndn_svs: Path,
    legacy: Path,
    output: Path,
) -> str:
    value = json.loads(lock.read_text())
    excludes = list(value.get("excludedContent", []))
    staging = output.with_name(output.name + f".partial.{os.getpid()}")
    if output.exists() or staging.exists():
        raise SealError("LAYER_SEAL_OUTPUT_EXISTS")
    (staging / "archives").mkdir(parents=True)
    try:
        legacy_value = json.loads((legacy / "source-seal.json").read_text())
        ndnsd_lock = value.get("sources", {}).get("NDNSD", {})
        ndnsd_legacy = legacy_value.get("dependencies", {}).get("NDNSD", {})
        if ndnsd_lock.get("revision") != ndnsd_legacy.get("revision"):
            raise SealError("LAYER_SEAL_LEGACY_REVISION_MISMATCH:NDNSD")
        ndnsd_source = legacy / str(ndnsd_legacy.get("archivePath", ""))
        ndnsd_target = staging / "archives" / "NDNSD.tar"
        shutil.copyfile(ndnsd_source, ndnsd_target)
        validate_archive(ndnsd_target)
        if sha256(ndnsd_target) != ndnsd_legacy.get("archiveDigest"):
            raise SealError("LAYER_SEAL_LEGACY_ARCHIVE_MISMATCH:NDNSD")
        records = {
            "NDNSD": {
                "revision": ndnsd_lock["revision"],
                "archivePath": "archives/NDNSD.tar",
                "archiveBytes": ndnsd_target.stat().st_size,
                "archiveDigest": sha256(ndnsd_target),
                "dirty": False,
            },
            "ndn-svs": snapshot_git_tree(
                ndn_svs, staging / "archives" / "ndn-svs.tar", excludes
            ),
            "ndnsf-workspace": snapshot_git_tree(
                workspace,
                staging / "archives" / "ndnsf-workspace.tar",
                excludes,
                list(value.get("workspaceIncludeRoots", [])),
            ),
        }
        manifest: dict[str, object] = {
            "schemaVersion": SCHEMA,
            "layer": "app-runtime",
            "createdAt": NORMALIZED_CREATED_AT,
            "lockDigest": sha256(lock),
            "sources": records,
            "developmentCandidate": any(
                bool(row["dirty"]) for row in records.values()
            ),
        }
        manifest["sealDigest"] = body_digest(manifest)
        (staging / "seal.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )
        os.replace(staging, output)
        return str(manifest["sealDigest"])
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def verify(lock: Path, output: Path) -> str:
    manifest = json.loads((output / "seal.json").read_text())
    if manifest.get("schemaVersion") != SCHEMA:
        raise SealError("LAYER_SEAL_SCHEMA_INVALID")
    if manifest.get("lockDigest") != sha256(lock):
        raise SealError("LAYER_SEAL_LOCK_MISMATCH")
    if manifest.get("sealDigest") != body_digest(manifest):
        raise SealError("LAYER_SEAL_MANIFEST_TAMPERED")
    sources = manifest.get("sources")
    if not isinstance(sources, dict) or not sources:
        raise SealError("LAYER_SEAL_SOURCES_INVALID")
    for name, row in sorted(sources.items()):
        archive = output / str(row.get("archivePath", ""))
        validate_archive(archive)
        if row.get("archiveBytes") != archive.stat().st_size:
            raise SealError(f"LAYER_SEAL_ARCHIVE_SIZE_MISMATCH:{name}")
        if row.get("archiveDigest") != sha256(archive):
            raise SealError(f"LAYER_SEAL_ARCHIVE_MISMATCH:{name}")
    return str(manifest["sealDigest"])


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="action", required=True)
    ndn = sub.add_parser("create-ndn")
    ndn.add_argument("--lock", required=True)
    ndn.add_argument("--legacy-seal", required=True)
    ndn.add_argument("--output", required=True)
    app = sub.add_parser("create-app")
    app.add_argument("--lock", required=True)
    app.add_argument("--workspace", required=True)
    app.add_argument("--ndn-svs", required=True)
    app.add_argument("--legacy-seal", required=True)
    app.add_argument("--output", required=True)
    check = sub.add_parser("verify")
    check.add_argument("--lock", required=True)
    check.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        if args.action == "create-ndn":
            result = create_ndn(
                Path(args.lock).resolve(),
                Path(args.legacy_seal).resolve(),
                Path(args.output).resolve(),
            )
        elif args.action == "create-app":
            result = create_app(
                Path(args.lock).resolve(),
                Path(args.workspace).resolve(),
                Path(args.ndn_svs).resolve(),
                Path(args.legacy_seal).resolve(),
                Path(args.output).resolve(),
            )
        else:
            result = verify(Path(args.lock).resolve(), Path(args.output).resolve())
    except (OSError, ValueError, KeyError, json.JSONDecodeError, SealError) as error:
        print(str(error), file=os.sys.stderr)
        return 4
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
