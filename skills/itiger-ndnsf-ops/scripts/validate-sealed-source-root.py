#!/usr/bin/env python3
"""Fail-closed validation of the directory consumed by rootless-build.sh.

The rootless wrapper reads dependency archives from ``.spec110-build`` while
the source-seal producer writes them at the staging root.  Validate the final
staging layout, including both copies, before submitting a Slurm build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


SCHEMA = "spec110-oci-source-seal-v1"


class ValidationError(RuntimeError):
    """Stable, fail-closed validation error."""


def digest_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValidationError(reason)


def load_seal(path: Path) -> tuple[dict, bytes]:
    require(path.is_file(), "SEALED_SOURCE_ROOT_MANIFEST_MISSING")
    raw = path.read_bytes()
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError("SEALED_SOURCE_ROOT_MANIFEST_INVALID_JSON") from error
    require(isinstance(value, dict), "SEALED_SOURCE_ROOT_MANIFEST_INVALID")
    expected = value.get("sealDigest")
    body = dict(value)
    body.pop("sealDigest", None)
    encoded = json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    require(value.get("schemaVersion") == SCHEMA, "SEALED_SOURCE_ROOT_SCHEMA_MISMATCH")
    require(expected == digest_bytes(encoded), "SEALED_SOURCE_ROOT_MANIFEST_DIGEST_MISMATCH")
    return value, raw


def validate_archive(path: Path, row: dict, label: str) -> None:
    require(path.is_file(), f"SEALED_SOURCE_ROOT_ARCHIVE_MISSING:{label}")
    require(path.stat().st_size == row.get("archiveBytes"),
            f"SEALED_SOURCE_ROOT_ARCHIVE_SIZE_MISMATCH:{label}")
    require(digest_file(path) == row.get("archiveDigest"),
            f"SEALED_SOURCE_ROOT_ARCHIVE_DIGEST_MISMATCH:{label}")


def validate(root: Path, lock: Path | None) -> dict[str, object]:
    seal_path = root / "source-seal.json"
    seal, raw_manifest = load_seal(seal_path)
    mirror = root / ".spec110-build"
    mirror_manifest = mirror / "source-seal.json"
    require(mirror_manifest.is_file(), "SEALED_SOURCE_ROOT_MIRROR_MANIFEST_MISSING")
    require(mirror_manifest.read_bytes() == raw_manifest,
            "SEALED_SOURCE_ROOT_MIRROR_MANIFEST_MISMATCH")

    if lock is not None:
        require(lock.is_file(), "SEALED_SOURCE_ROOT_LOCK_MISSING")
        require(digest_file(lock) == seal.get("lockDigest"),
                "SEALED_SOURCE_ROOT_LOCK_DIGEST_MISMATCH")

    workspace = seal.get("workspace")
    require(isinstance(workspace, dict), "SEALED_SOURCE_ROOT_WORKSPACE_RECORD_MISSING")
    workspace_archive = root / "workspace.tar"
    validate_archive(workspace_archive, workspace, "workspace")

    dependencies = seal.get("dependencies")
    require(isinstance(dependencies, dict) and dependencies,
            "SEALED_SOURCE_ROOT_DEPENDENCIES_MISSING")
    for name, row in sorted(dependencies.items()):
        require(isinstance(row, dict), f"SEALED_SOURCE_ROOT_DEPENDENCY_RECORD_INVALID:{name}")
        relative = row.get("archivePath")
        require(isinstance(relative, str) and relative.startswith("archives/")
                and ".." not in Path(relative).parts,
                f"SEALED_SOURCE_ROOT_ARCHIVE_PATH_INVALID:{name}")
        # The root copy is useful for inspection; the mirror is what the
        # rootless builder actually reads.  Both must be byte-identical.
        validate_archive(root / relative, row, name)
        validate_archive(mirror / relative, row, f"{name}:mirror")
        require((root / relative).read_bytes() == (mirror / relative).read_bytes(),
                f"SEALED_SOURCE_ROOT_ARCHIVE_MIRROR_MISMATCH:{name}")

    return {
        "status": "PASS",
        "schemaVersion": "itiger-sealed-source-root-preflight-v1",
        "sourceRevision": workspace.get("revision"),
        "sealDigest": seal.get("sealDigest"),
        "dependencyCount": len(dependencies),
        "workspaceBytes": workspace.get("archiveBytes"),
        "workspaceDigest": digest_file(workspace_archive),
        "mirror": str(mirror),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--lock")
    args = parser.parse_args()
    try:
        report = validate(Path(args.source_root).resolve(),
                          Path(args.lock).resolve() if args.lock else None)
    except (OSError, KeyError, TypeError, ValueError, ValidationError) as error:
        report = {"status": "FAIL", "reasonCode": str(error)}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
