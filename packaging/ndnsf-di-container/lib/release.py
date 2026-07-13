"""Immutable OCI release and runtime-materialization helpers."""

from __future__ import annotations

import hashlib
import json
import os
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


def create_immutable_gpu_release_record(*, manifest_path: Path | str,
                                        signature_bundle_path: Path | str,
                                        output_path: Path | str) -> dict[str, Any]:
    """Bind one externally verified OCI signature bundle to a GPU release."""
    manifest_source = Path(manifest_path)
    signature_source = Path(signature_bundle_path)
    manifest = load_release_manifest(manifest_source)
    if re.fullmatch(r"[a-f0-9]{40}", manifest.get("sourceRevision", "")) is None:
        raise ReleaseError("SPEC110_RELEASE_SOURCE_REVISION_INVALID")
    gpu_images = [image for image in manifest["images"].values()
                  if image["backend"] == "onnxruntime-cuda"]
    if len(gpu_images) != 1:
        raise ReleaseError("SPEC110_RELEASE_GPU_IMAGE_COUNT_INVALID")
    image = gpu_images[0]
    try:
        signature = json.loads(signature_source.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReleaseError(f"SPEC110_RELEASE_SIGNATURE_READ_FAILED:{exc}") from exc
    required = {"schemaVersion", "verified", "imageDigest", "issuer", "subject",
                "transparencyLog", "visibility", "authMode"}
    if not isinstance(signature, dict) or set(signature) != required:
        raise ReleaseError("SPEC110_RELEASE_SIGNATURE_FIELDS_INVALID")
    if signature["schemaVersion"] != "cosign-verification-bundle-v1" or signature["verified"] is not True:
        raise ReleaseError("SPEC110_RELEASE_SIGNATURE_NOT_VERIFIED")
    if signature["imageDigest"] != image["digest"]:
        raise ReleaseError("SPEC110_RELEASE_SIGNATURE_DIGEST_MISMATCH")
    if not signature["issuer"] or not signature["subject"] or not signature["transparencyLog"]:
        raise ReleaseError("SPEC110_RELEASE_SIGNATURE_IDENTITY_INVALID")
    body = {
        "schemaVersion": "spec110-runtime-release-v1",
        "releaseId": manifest["releaseId"],
        "sourceRevision": manifest["sourceRevision"],
        "imageReference": image["reference"],
        "imageDigest": image["digest"],
        "manifestDigest": sha256_file(manifest_source),
        "sbomDigest": manifest["sbom"]["digest"],
        "provenanceDigest": manifest["provenance"]["digest"],
        "signatureBundleDigest": sha256_file(signature_source),
        "signatureIdentity": {"issuer": signature["issuer"], "subject": signature["subject"]},
        "transparencyLog": signature["transparencyLog"],
        "visibility": signature["visibility"],
        "authMode": signature["authMode"],
        "immutable": True,
        "physicalProduction": "DEFERRED",
    }
    body["recordDigest"] = manifest_digest(body)
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o640)
    except FileExistsError as exc:
        raise ReleaseError("SPEC110_RELEASE_RECORD_EXISTS") from exc
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(body, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    return body


def validate_immutable_gpu_release_record(value: dict[str, Any]) -> dict[str, Any]:
    if value.get("schemaVersion") != "spec110-runtime-release-v1":
        raise ReleaseError("SPEC110_RELEASE_RECORD_SCHEMA_INVALID")
    if value.get("immutable") is not True or value.get("physicalProduction") != "DEFERRED":
        raise ReleaseError("SPEC110_RELEASE_RECORD_AUTHORITY_INVALID")
    if REFERENCE_RE.fullmatch(str(value.get("imageReference", ""))) is None:
        raise ReleaseError("SPEC110_RELEASE_RECORD_IMAGE_INVALID")
    if value.get("imageReference", "").rsplit("@", 1)[-1] != value.get("imageDigest"):
        raise ReleaseError("SPEC110_RELEASE_RECORD_IMAGE_MISMATCH")
    for field in ("imageDigest", "manifestDigest", "sbomDigest", "provenanceDigest",
                  "signatureBundleDigest", "recordDigest"):
        if DIGEST_RE.fullmatch(str(value.get(field, ""))) is None:
            raise ReleaseError(f"SPEC110_RELEASE_RECORD_DIGEST_INVALID:{field}")
    body = dict(value)
    actual = body.pop("recordDigest")
    if manifest_digest(body) != actual:
        raise ReleaseError("SPEC110_RELEASE_RECORD_TAMPERED")
    return value
