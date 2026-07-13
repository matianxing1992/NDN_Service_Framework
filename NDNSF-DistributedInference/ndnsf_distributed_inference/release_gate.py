"""Mechanical Spec 105 MiniNDN candidate release gate."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from .runtime_v1 import ExecutionEvidenceV1, classify_execution_evidence
from .runtime_v1_evidence import (
    SPEC107_RELEASE_DIMENSIONS,
    evaluate_spec107_release_input,
)


DIMENSIONS = (
    "evidenceIntegrity", "correctness", "performance",
    "applicationSecurity", "recovery", "operations",
)


def verify_evidence_manifest(payload: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    root_value = payload.get("evidence_root", "")
    manifest = payload.get("evidence_manifest")
    if not root_value or not isinstance(manifest, list) or not manifest:
        return ["EVIDENCE_MANIFEST_MISSING"]
    root = Path(str(root_value)).resolve()
    observed: set[str] = set()
    for index, item in enumerate(manifest):
        if not isinstance(item, dict):
            errors.append(f"EVIDENCE_MANIFEST_ENTRY_INVALID:{index}")
            continue
        relative = str(item.get("path", ""))
        expected = str(item.get("sha256", "")).lower()
        path = (root / relative).resolve()
        try:
            path.relative_to(root)
        except ValueError:
            errors.append(f"EVIDENCE_PATH_ESCAPE:{relative}")
            continue
        if not path.is_file():
            errors.append(f"EVIDENCE_MISSING:{relative}")
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if not expected.startswith("sha256:") or actual != expected[7:]:
            errors.append(f"EVIDENCE_DIGEST_MISMATCH:{relative}")
            continue
        observed.add(relative)
    required = {
        str(path)
        for dimension in payload.get("dimensions", {}).values()
        if isinstance(dimension, dict)
        for path in dimension.get("artifacts", [])
    }
    for relative in sorted(required - observed):
        errors.append(f"EVIDENCE_UNBOUND:{relative}")
    return errors


def build_release_gate(*, release_id: str, source_commit: str,
                       profile_digest: str,
                       dimensions: dict[str, dict[str, Any]],
                       execution_evidence: list[dict[str, Any]],
                       limitations: list[str] | None = None,
                       generated_at_ms: int = 0) -> dict[str, Any]:
    evidence_status = "BLOCK"
    classification = "invalid-evidence"
    try:
        parsed = [ExecutionEvidenceV1.from_dict(item) for item in execution_evidence]
        classification = classify_execution_evidence(parsed)
        if classification in {"onnxruntime-cpu", "onnxruntime-cuda",
                              "transformers", "llama-server"}:
            evidence_status = "PASS"
    except (TypeError, ValueError):
        pass
    normalized: dict[str, dict[str, Any]] = {}
    for name in DIMENSIONS:
        value = dict(dimensions.get(name, {}))
        status = str(value.get("status", "BLOCK")).upper()
        artifacts = value.get("artifacts", [])
        if status != "PASS" or not isinstance(artifacts, list) or not artifacts:
            status = "BLOCK"
        normalized[name] = {**value, "status": status, "artifacts": artifacts}
    normalized["evidenceIntegrity"]["status"] = (
        "PASS" if evidence_status == "PASS" and
        normalized["evidenceIntegrity"]["status"] == "PASS" else "BLOCK"
    )
    overall = "PASS" if all(item["status"] == "PASS" for item in normalized.values()) else "BLOCK"
    return {
        "schema": "ndnsf-di-release-gate-v1",
        "releaseId": release_id,
        "sourceCommit": source_commit,
        "profileDigest": profile_digest,
        "runnerClassification": classification,
        "dimensions": normalized,
        "minindnCandidateOverall": overall,
        "physicalProductionOverall": "DEFERRED",
        "physicalAcceptanceSpec": "specs/106-ndnsf-di-physical-pilot",
        "limitations": list(limitations or []),
        "generatedAtMs": int(generated_at_ms),
    }


def build_spec107_release_gate(payload: dict[str, Any], *,
                               evidence_root: str | Path) -> dict[str, Any]:
    """Build the successor gate while retaining the frozen predecessor decision."""

    evaluation = evaluate_spec107_release_input(
        payload, evidence_root=evidence_root)
    raw_dimensions = payload.get("dimensions")
    raw_dimensions = raw_dimensions if isinstance(raw_dimensions, dict) else {}
    dimensions: dict[str, dict[str, Any]] = {}
    for name in SPEC107_RELEASE_DIMENSIONS:
        value = raw_dimensions.get(name)
        value = dict(value) if isinstance(value, dict) else {}
        status = "PASS" if value.get("status") == "PASS" else "BLOCK"
        dimensions[name] = {
            **value,
            "status": status,
            "artifacts": list(value.get("artifacts", []))
            if isinstance(value.get("artifacts"), list) else [],
        }
    predecessor = {
        "releaseId": "spec105-local-minindn-candidate-r2",
        "minindnCandidateOverall": "BLOCK",
        "physicalProductionOverall": "DEFERRED",
    }
    overall = "PASS" if evaluation["eligible"] else "BLOCK"
    return {
        "schema": "ndnsf-di-spec107-release-gate-v1",
        "candidateId": evaluation["candidateId"],
        "predecessor": predecessor,
        "predecessorObserved": payload.get("predecessor"),
        "dimensions": dimensions,
        "evidenceManifest": list(payload.get("evidenceManifest", []))
        if isinstance(payload.get("evidenceManifest"), list) else [],
        "errors": evaluation["errors"],
        "minindnCandidateOverall": overall,
        "physicalProductionOverall": "DEFERRED",
        "physicalAcceptanceSpec": "specs/106-ndnsf-di-physical-pilot",
        "limitations": list(payload.get("limitations", []))
        if isinstance(payload.get("limitations"), list) else [],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    payload = json.loads(args.input.read_text(encoding="utf-8"))
    manifest_errors = verify_evidence_manifest(payload)
    build_input = {
        key: value for key, value in payload.items()
        if key not in {"evidence_root", "evidence_manifest", "gate_generator_commit"}
    }
    gate = build_release_gate(**build_input)
    gate["evidenceManifest"] = list(payload.get("evidence_manifest", []))
    gate["gateGeneratorCommit"] = str(payload.get("gate_generator_commit", ""))
    gate["evidenceManifestErrors"] = manifest_errors
    if manifest_errors or not gate["gateGeneratorCommit"]:
        gate["dimensions"]["evidenceIntegrity"]["status"] = "BLOCK"
        gate["minindnCandidateOverall"] = "BLOCK"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(gate, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if gate["minindnCandidateOverall"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
