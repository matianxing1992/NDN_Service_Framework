#!/usr/bin/env python3
"""Fail-closed validation of the local Spec170 SIF build provenance."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


RECORD_SCHEMA = "ndnsf-local-sif-build-v3"
BOUNDARY_SCHEMA = "spec170-sif-build-boundary-v2"
DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")


class BuildRecordError(ValueError):
    """The release record cannot prove a container-native build."""


def _fail(code: str, detail: object = "") -> None:
    suffix = f":{detail}" if detail != "" else ""
    raise BuildRecordError(code + suffix)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _record_digest(record: dict[str, Any]) -> str:
    unsigned = dict(record)
    unsigned.pop("recordDigest", None)
    return "sha256:" + hashlib.sha256(
        json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def validate(record_path: Path | str, sif_path: Path | str,
             expected_sha256: str, *, verify_sif_hash: bool = True) -> dict[str, Any]:
    record_file = Path(record_path).resolve()
    sif = Path(sif_path).resolve()
    if not DIGEST.fullmatch(expected_sha256):
        _fail("SPEC170_BUILD_RECORD_EXPECTED_SIF_DIGEST_INVALID")
    if not record_file.is_file():
        _fail("SPEC170_BUILD_RECORD_MISSING", record_file)
    if verify_sif_hash and not sif.is_file():
        _fail("SPEC170_BUILD_RECORD_SIF_MISSING", sif)
    try:
        record = json.loads(record_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        _fail("SPEC170_BUILD_RECORD_INVALID", type(error).__name__)
    if not isinstance(record, dict):
        _fail("SPEC170_BUILD_RECORD_INVALID")
    if record.get("schemaVersion") != RECORD_SCHEMA:
        _fail("SPEC170_BUILD_RECORD_SCHEMA_MISMATCH")
    if record.get("status") != "PASS":
        _fail("SPEC170_BUILD_RECORD_NOT_PASS")
    if record.get("recordDigest") != _record_digest(record):
        _fail("SPEC170_BUILD_RECORD_DIGEST_MISMATCH")

    source_validation = record.get("sourceValidation")
    if not isinstance(source_validation, dict) or source_validation.get("status") != "PASS":
        _fail("SPEC170_BUILD_RECORD_SOURCE_VALIDATION_MISSING")

    boundary = record.get("containerNativeBuild")
    if not isinstance(boundary, dict):
        _fail("SPEC170_BUILD_RECORD_BOUNDARY_MISSING")
    required_boundary = {
        "status": "PASS",
        "schemaVersion": BOUNDARY_SCHEMA,
        "containerNativeBuild": True,
        "staleBaseArtifactsReplaced": True,
        "hostBinaryInputs": [],
    }
    for key, expected in required_boundary.items():
        if boundary.get(key) != expected:
            _fail("SPEC170_BUILD_RECORD_BOUNDARY_INVALID", key)
    if record.get("hostRole") != "apptainer-driver-only":
        _fail("SPEC170_BUILD_RECORD_HOST_ROLE_INVALID")
    build_input = record.get("buildInput")
    if not isinstance(build_input, dict) or build_input.get("method") != "local-apptainer-definition":
        _fail("SPEC170_BUILD_RECORD_BUILD_METHOD_INVALID")

    recorded_sif = record.get("sif")
    if not isinstance(recorded_sif, dict) or recorded_sif.get("sha256") != expected_sha256:
        _fail("SPEC170_BUILD_RECORD_SIF_DIGEST_MISMATCH")
    if verify_sif_hash:
        if (not isinstance(recorded_sif.get("bytes"), int)
                or recorded_sif["bytes"] != sif.stat().st_size):
            _fail("SPEC170_BUILD_RECORD_SIF_SIZE_MISMATCH")
    actual = expected_sha256
    if verify_sif_hash:
        actual = _sha256(sif)
        if actual != expected_sha256:
            _fail("SPEC170_BUILD_RECORD_SIF_HASH_MISMATCH", actual)

    return {
        "status": "PASS",
        "record": str(record_file),
        "sif": str(sif),
        "sifSha256": actual,
        "schemaVersion": RECORD_SCHEMA,
        "containerNativeBuild": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", required=True, type=Path)
    parser.add_argument("--sif", required=True, type=Path)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument(
        "--metadata-only",
        action="store_true",
        help=(
            "Validate the immutable build record and SIF size without reading "
            "the full SIF; the caller must hash the staged SIF before execution."
        ),
    )
    args = parser.parse_args()
    try:
        print(json.dumps(validate(
            args.record,
            args.sif,
            args.expected_sha256,
            verify_sif_hash=not args.metadata_only,
        ),
                          sort_keys=True, separators=(",", ":")))
    except BuildRecordError as error:
        print(str(error), file=__import__("sys").stderr)
        return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
