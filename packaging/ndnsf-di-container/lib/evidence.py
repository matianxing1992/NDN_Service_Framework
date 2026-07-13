"""Canonical, fail-closed Spec 108 deployment evidence handling."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Any, Iterable

from redaction import redact, secret_findings
from schema_utils import SchemaValidationError, validate_schema


class EvidenceError(ValueError):
    """Evidence is invalid, unsafe, incomplete, or overclaims authority."""


def validate_evidence(value: dict[str, Any]) -> dict[str, Any]:
    try:
        validate_schema(value, "deployment-evidence.schema.json")
    except SchemaValidationError as exc:
        raise EvidenceError(f"EVIDENCE_INVALID:{exc}") from exc
    if value["authority"]["physicalProduction"] != "DEFERRED":
        raise EvidenceError("EVIDENCE_PHYSICAL_PRODUCTION_MUST_BE_DEFERRED")
    if value["backend"]["fallbackOccurred"] and value["outcome"] == "PASS":
        raise EvidenceError("EVIDENCE_FALLBACK_CANNOT_PASS")
    findings = secret_findings(value)
    if findings:
        raise EvidenceError("EVIDENCE_SECRET_FINDINGS:" + ",".join(findings[:5]))
    return value


def canonical_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def canonical_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def initialize_evidence(*, run_id: str, candidate: dict[str, Any], release: dict[str, Any],
                        profile_digest: str, started_at: str) -> dict[str, Any]:
    return {
        "schemaVersion": "1.0", "runId": run_id, "candidate": candidate,
        "release": release, "profileDigest": profile_digest, "tests": [],
        "authority": {"substrate": "DEFERRED", "candidate": "DEFERRED",
                      "physicalProduction": "DEFERRED", "physicalProductionOwner": "Spec 106"},
        "startedAt": started_at,
    }


def finalize_evidence(value: dict[str, Any], *, finished_at: str, outcome: str) -> dict[str, Any]:
    result = redact(dict(value))
    result["finishedAt"] = finished_at
    result["outcome"] = outcome
    findings = secret_findings(result)
    result["redaction"] = {"status": "PASS" if not findings else "FAIL", "scanner": "spec108-redaction-v1", "findings": len(findings)}
    return validate_evidence(result)


def promote_evidence(files: Iterable[Path | str], destination: Path | str) -> dict[str, Any]:
    target = Path(destination)
    if target.exists():
        raise EvidenceError(f"EVIDENCE_DESTINATION_EXISTS:{target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{target.name}.", dir=str(target.parent)))
    manifest: dict[str, str] = {}
    try:
        for source_value in files:
            source = Path(source_value)
            if not source.is_file():
                raise EvidenceError(f"EVIDENCE_SOURCE_NOT_FILE:{source}")
            if source.name in manifest:
                raise EvidenceError(f"EVIDENCE_DUPLICATE_BASENAME:{source.name}")
            destination_file = staging / source.name
            shutil.copy2(source, destination_file)
            manifest[source.name] = "sha256:" + hashlib.sha256(destination_file.read_bytes()).hexdigest()
        manifest_path = staging / "promotion-manifest.json"
        manifest_path.write_bytes(canonical_bytes({"files": manifest}))
        os.replace(staging, target)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return {"destination": str(target), "files": manifest,
            "manifestDigest": "sha256:" + hashlib.sha256((target / "promotion-manifest.json").read_bytes()).hexdigest()}
