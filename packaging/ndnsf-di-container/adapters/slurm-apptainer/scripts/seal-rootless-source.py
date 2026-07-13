#!/usr/bin/env python3
"""Create or verify the exact source seal consumed by rootless iTiger builds."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess


class SealError(RuntimeError):
    pass


SCHEMA = "spec110-oci-source-seal-v1"
ARCHIVE_CONFIG = (
    "-c", "filter.lfs.process=",
    "-c", "filter.lfs.smudge=",
    "-c", "filter.lfs.required=false",
)


def run(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args], text=True, capture_output=True, check=False
    )
    if result.returncode:
        raise SealError(f"SOURCE_SEAL_GIT_FAILED:{repo.name}:{args[0]}")
    return result.stdout.strip()


def archive_measure(repo: Path, revision: str) -> dict[str, object]:
    process = subprocess.Popen(
        ["git", "-C", str(repo), *ARCHIVE_CONFIG, "archive", "--format=tar", revision],
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
        raise SealError(f"SOURCE_SEAL_ARCHIVE_FAILED:{repo.name}:{stderr.decode(errors='replace')}")
    return {"archiveDigest": "sha256:" + digest.hexdigest(), "archiveBytes": count}


def repo_record(repo: Path, expected: str | None = None) -> dict[str, object]:
    revision = run(repo, "rev-parse", "HEAD")
    if expected is not None and revision != expected:
        raise SealError(f"SOURCE_SEAL_REVISION_MISMATCH:{repo.name}")
    if run(repo, "status", "--porcelain", "--untracked-files=no"):
        raise SealError(f"SOURCE_SEAL_TRACKED_DIRTY:{repo.name}")
    return {"revision": revision, **archive_measure(repo, revision)}


def body_digest(value: dict) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def evaluate(source_root: Path, lock_path: Path) -> dict:
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    sources = lock.get("sourceRepositories")
    if lock.get("schemaVersion") != "ndnsf-di-gpu-lock-v1" or not isinstance(sources, dict):
        raise SealError("SOURCE_SEAL_LOCK_INVALID")
    workspace = source_root / "workspace"
    dependencies = source_root / "dependencies"
    return {
        "workspace": repo_record(workspace),
        "dependencies": {
            name: {
                **repo_record(dependencies / name, row["revision"]),
                "archivePath": f"archives/{name}.tar",
            }
            for name, row in sorted(sources.items())
        },
        "lockDigest": "sha256:" + hashlib.sha256(lock_path.read_bytes()).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--lock", required=True)
    parser.add_argument("--manifest")
    args = parser.parse_args()
    source_root = Path(args.source_root).resolve()
    lock = Path(args.lock).resolve()
    manifest = Path(args.manifest).resolve() if args.manifest else source_root / "source-seal.json"
    measured = evaluate(source_root, lock)
    if args.action == "create":
        body = {
            "schemaVersion": SCHEMA,
            "createdAt": dt.datetime.now(dt.timezone.utc).isoformat(),
            **measured,
        }
        body["sealDigest"] = body_digest(body)
        manifest.parent.mkdir(parents=True, exist_ok=True)
        try:
            descriptor = os.open(manifest, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o640)
        except FileExistsError as exc:
            raise SealError("SOURCE_SEAL_MANIFEST_EXISTS") from exc
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(body, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        print(body["sealDigest"])
        return 0
    value = json.loads(manifest.read_text(encoding="utf-8"))
    actual = dict(value)
    digest = actual.pop("sealDigest", None)
    if value.get("schemaVersion") != SCHEMA or body_digest(actual) != digest:
        raise SealError("SOURCE_SEAL_MANIFEST_TAMPERED")
    for field in ("workspace", "dependencies", "lockDigest"):
        if value.get(field) != measured[field]:
            raise SealError(f"SOURCE_SEAL_CONTENT_MISMATCH:{field}")
    print(digest)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, json.JSONDecodeError, KeyError, SealError) as error:
        print(str(error), file=__import__("sys").stderr)
        raise SystemExit(4)
