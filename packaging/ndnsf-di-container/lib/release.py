"""Immutable OCI release and runtime-materialization helpers."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from typing import Any

from schema_utils import SchemaValidationError, validate_schema


class ReleaseError(ValueError):
    """Release manifest or artifact identity failure."""


DIGEST_RE = re.compile(r"^sha256:([a-f0-9]{64})$")
REFERENCE_RE = re.compile(r"^\S+@sha256:([a-f0-9]{64})$")


def sha256_file(path: Path | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def validate_release_manifest(value: dict[str, Any]) -> dict[str, Any]:
    try:
        validate_schema(value, "oci-release.schema.json")
    except SchemaValidationError as exc:
        raise ReleaseError(f"RELEASE_INVALID:{exc}") from exc
    for name, image in value["images"].items():
        reference = REFERENCE_RE.fullmatch(image["reference"])
        digest = DIGEST_RE.fullmatch(image["digest"])
        if reference is None or digest is None:
            raise ReleaseError(f"RELEASE_IMAGE_NOT_DIGEST_PINNED:{name}")
        if reference.group(1) != digest.group(1):
            raise ReleaseError(f"RELEASE_IMAGE_DIGEST_MISMATCH:{name}")
    return value


def load_release_manifest(path: Path | str) -> dict[str, Any]:
    source = Path(path)
    try:
        value = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReleaseError(f"RELEASE_READ_FAILED:{source}:{exc}") from exc
    if not isinstance(value, dict):
        raise ReleaseError("RELEASE_INVALID:$: expected object")
    return validate_release_manifest(value)


def manifest_digest(value: dict[str, Any]) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def materialization_record(*, adapter: str, oci_reference: str, materialization_type: str,
                           materialization_id: str, runtime_version: str,
                           path: str | None = None) -> dict[str, Any]:
    reference = REFERENCE_RE.fullmatch(oci_reference)
    if reference is None:
        raise ReleaseError("MATERIALIZATION_OCI_NOT_DIGEST_PINNED")
    if adapter == "slurm-apptainer":
        if materialization_type != "sif" or DIGEST_RE.fullmatch(materialization_id) is None or not path:
            raise ReleaseError("MATERIALIZATION_SIF_IDENTITY_INVALID")
    elif adapter == "docker-compose":
        if materialization_type != "docker-image":
            raise ReleaseError("MATERIALIZATION_DOCKER_IDENTITY_INVALID")
    else:
        raise ReleaseError(f"MATERIALIZATION_ADAPTER_UNKNOWN:{adapter}")
    return {
        "adapter": adapter,
        "ociReference": oci_reference,
        "ociDigest": "sha256:" + reference.group(1),
        "type": materialization_type,
        "id": materialization_id,
        "path": path,
        "runtimeVersion": runtime_version,
        "verified": True,
    }
