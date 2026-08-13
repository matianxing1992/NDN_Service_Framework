#!/usr/bin/env python3
"""Validate the checksum-bound source closure used by Spec 167 jobs."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


REQUIRED_RUNTIME_FILES = (
    "Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py",
    "Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py",
    "Experiments/analyze_spec167_itiger_repo.py",
    "Experiments/spec164_artifact_campaign.py",
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_source_bundle(source_root: Path, manifest_path: Path) -> list[str]:
    """Return deterministic validation errors; an empty list means PASS."""

    try:
        manifest: dict[str, Any] = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return [f"manifest unreadable: {error}"]
    entries = manifest.get("files")
    if not isinstance(entries, list):
        return ["manifest.files must be a list"]

    errors: list[str] = []
    by_path: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            errors.append("manifest contains an invalid file entry")
            continue
        relative = entry["path"]
        candidate = Path(relative)
        if candidate.is_absolute() or ".." in candidate.parts:
            errors.append(f"unsafe manifest path: {relative}")
            continue
        if relative in by_path:
            errors.append(f"duplicate manifest path: {relative}")
            continue
        by_path[relative] = entry
        actual = source_root / candidate
        if not actual.is_file():
            errors.append(f"manifest file missing from source root: {relative}")
            continue
        if entry.get("bytes") != actual.stat().st_size:
            errors.append(f"size mismatch: {relative}")
        if entry.get("sha256") != _sha256(actual):
            errors.append(f"sha256 mismatch: {relative}")

    for relative in REQUIRED_RUNTIME_FILES:
        if relative not in by_path:
            errors.append(f"runtime dependency absent from manifest: {relative}")
        elif not (source_root / relative).is_file():
            errors.append(f"runtime dependency absent from source root: {relative}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    errors = validate_source_bundle(args.source_root, args.manifest)
    if errors:
        for error in errors:
            print(f"SPEC167_SOURCE_BUNDLE_FAIL: {error}")
        return 1
    print("SPEC167_SOURCE_BUNDLE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
