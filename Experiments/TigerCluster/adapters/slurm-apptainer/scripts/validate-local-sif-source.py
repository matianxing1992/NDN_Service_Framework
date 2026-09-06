#!/usr/bin/env python3
"""Validate a Spec170 source-only SIF build input before Apptainer runs."""

from __future__ import annotations

import argparse
import hashlib
import json
import tarfile
from pathlib import Path, PurePosixPath


SCHEMA = "spec170-local-sif-source-v1"
SEAL_DIGEST_BASIS = "path-independent-content-v1"
EXCLUDED_DIRS = {"build", "__pycache__", "node_modules"}
EXCLUDED_SUFFIXES = {".so", ".a", ".o", ".pyc", ".pyo"}


class ValidationError(RuntimeError):
    pass


def fail(reason: str) -> None:
    raise ValidationError(reason)


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def canonical_seal_body(body: dict) -> dict:
    """Normalize operational paths before checking a content seal.

    Older v1 seals omitted ``sealDigestBasis`` and remain path-bound for
    compatibility.  New seals explicitly opt into this path-independent
    basis so relocation does not change source identity.
    """
    canonical = json.loads(json.dumps(body, sort_keys=True))
    canonical.pop("sealDigest", None)
    canonical["workspace"] = "<workspace>"
    canonical["sealDigestBasis"] = SEAL_DIGEST_BASIS
    archive = canonical.get("archive")
    if isinstance(archive, dict):
        archive["path"] = "<archive:workspace>"
    dependencies = canonical.get("dependencies", {})
    if isinstance(dependencies, dict):
        for name, record in dependencies.items():
            if not isinstance(record, dict):
                continue
            record["workspace"] = f"<workspace:{name}>"
            dep_archive = record.get("archive")
            if isinstance(dep_archive, dict):
                dep_archive["path"] = f"<archive:{name}>"
    return canonical


def validate_rows(rows: object, label: str) -> dict[str, dict]:
    if not isinstance(rows, list) or not rows:
        fail(f"LOCAL_SIF_SOURCE_FILES_INVALID:{label}")
    by_path: dict[str, dict] = {}
    for row in rows:
        if not isinstance(row, dict):
            fail(f"LOCAL_SIF_SOURCE_FILE_RECORD_INVALID:{label}")
        name = row.get("path")
        if not isinstance(name, str) or not name:
            fail(f"LOCAL_SIF_SOURCE_FILE_PATH_INVALID:{label}")
        pure = PurePosixPath(name)
        if pure.is_absolute() or ".." in pure.parts or name in by_path:
            fail(f"LOCAL_SIF_SOURCE_FILE_PATH_INVALID:{label}:{name}")
        if (any(part in EXCLUDED_DIRS or part.endswith(".egg-info")
                for part in pure.parts) or pure.suffix in EXCLUDED_SUFFIXES):
            fail(f"LOCAL_SIF_SOURCE_COMPILED_PAYLOAD:{label}:{name}")
        if not isinstance(row.get("bytes"), int) or row["bytes"] < 0:
            fail(f"LOCAL_SIF_SOURCE_FILE_SIZE_INVALID:{label}:{name}")
        if not isinstance(row.get("sha256"), str):
            fail(f"LOCAL_SIF_SOURCE_FILE_DIGEST_INVALID:{label}:{name}")
        by_path[name] = row
    return by_path


def validate_archive(root: Path, record: object, rows: object,
                     expected_name: str, label: str) -> dict:
    if not isinstance(record, dict):
        fail(f"LOCAL_SIF_SOURCE_ARCHIVE_RECORD_INVALID:{label}")
    archive = root / expected_name
    recorded_path = record.get("path")
    if not isinstance(recorded_path, str) or Path(recorded_path).resolve() != archive.resolve():
        fail(f"LOCAL_SIF_SOURCE_ARCHIVE_PATH_MISMATCH:{label}")
    if not archive.is_file():
        fail(f"LOCAL_SIF_SOURCE_ARCHIVE_MISSING:{label}")
    if archive.stat().st_size != record.get("bytes"):
        fail(f"LOCAL_SIF_SOURCE_ARCHIVE_SIZE_MISMATCH:{label}")
    if digest_file(archive) != record.get("sha256"):
        fail(f"LOCAL_SIF_SOURCE_ARCHIVE_DIGEST_MISMATCH:{label}")

    expected = validate_rows(rows, label)
    observed: dict[str, dict] = {}
    try:
        with tarfile.open(archive, "r") as stream:
            for member in stream.getmembers():
                name = member.name
                if not member.isfile() or name in observed:
                    fail(f"LOCAL_SIF_SOURCE_ARCHIVE_MEMBER_INVALID:{label}:{name}")
                extracted = stream.extractfile(member)
                if extracted is None:
                    fail(f"LOCAL_SIF_SOURCE_ARCHIVE_MEMBER_INVALID:{label}:{name}")
                content = extracted.read()
                observed[name] = {
                    "bytes": len(content),
                    "sha256": digest_bytes(content),
                }
    except (tarfile.TarError, OSError) as error:
        fail(f"LOCAL_SIF_SOURCE_ARCHIVE_INVALID:{label}:{type(error).__name__}")
    if set(observed) != set(expected):
        fail(f"LOCAL_SIF_SOURCE_ARCHIVE_MEMBERS_MISMATCH:{label}")
    for name, row in expected.items():
        if observed[name] != {"bytes": row["bytes"], "sha256": row["sha256"]}:
            fail(f"LOCAL_SIF_SOURCE_ARCHIVE_MEMBER_MISMATCH:{label}:{name}")
    return {
        "path": str(archive.resolve()),
        "bytes": archive.stat().st_size,
        "sha256": digest_file(archive),
        "fileCount": len(expected),
    }


def validate(seal_path: Path) -> dict:
    try:
        seal = json.loads(seal_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"LOCAL_SIF_SOURCE_SEAL_INVALID:{type(error).__name__}")
    if not isinstance(seal, dict) or seal.get("schemaVersion") != SCHEMA:
        fail("LOCAL_SIF_SOURCE_SCHEMA_MISMATCH")
    expected_digest = seal.get("sealDigest")
    if seal.get("sealDigestBasis") == SEAL_DIGEST_BASIS:
        digest_body = canonical_seal_body(seal)
    else:
        # Preserve validation of existing path-bound v1 records.  New source
        # preparation always emits the path-independent basis above.
        digest_body = dict(seal)
        digest_body.pop("sealDigest", None)
    actual_digest = digest_bytes(json.dumps(
        digest_body, sort_keys=True, separators=(",", ":")).encode())
    if expected_digest != actual_digest:
        fail("LOCAL_SIF_SOURCE_SEAL_DIGEST_MISMATCH")
    if seal.get("compiledPayloadCount") != 0:
        fail("LOCAL_SIF_SOURCE_COMPILED_PAYLOAD_COUNT_NONZERO:workspace")
    if seal.get("fileCount") != len(seal.get("files", [])):
        fail("LOCAL_SIF_SOURCE_FILE_COUNT_MISMATCH:workspace")

    root = seal_path.resolve().parent
    archives = {
        "workspace": validate_archive(
            root, seal.get("archive"), seal.get("files"),
            "workspace.tar", "workspace")
    }
    dependencies = seal.get("dependencies", {})
    if not isinstance(dependencies, dict):
        fail("LOCAL_SIF_SOURCE_DEPENDENCIES_INVALID")
    for name, record in sorted(dependencies.items()):
        if not isinstance(record, dict):
            fail(f"LOCAL_SIF_SOURCE_DEPENDENCY_INVALID:{name}")
        if record.get("compiledPayloadCount") != 0:
            fail(f"LOCAL_SIF_SOURCE_COMPILED_PAYLOAD_COUNT_NONZERO:{name}")
        if record.get("fileCount") != len(record.get("files", [])):
            fail(f"LOCAL_SIF_SOURCE_FILE_COUNT_MISMATCH:{name}")
        filename = "ndn-svs.tar" if name == "ndnSvs" else f"{name}.tar"
        archives[name] = validate_archive(
            root, record.get("archive"), record.get("files"), filename, name)
    return {
        "schemaVersion": "spec170-local-sif-source-preflight-v1",
        "status": "PASS",
        "sourceRevision": seal.get("sourceRevision"),
        "sealDigest": expected_digest,
        "archives": archives,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-seal", required=True, type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.source_seal), sort_keys=True))
    except ValidationError as error:
        print(str(error), file=__import__("sys").stderr)
        return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
