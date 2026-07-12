"""Mechanical Spec 105 MiniNDN candidate release gate."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from .runtime_v1 import ExecutionEvidenceV1, classify_execution_evidence


DIMENSIONS = (
    "evidenceIntegrity", "correctness", "performance",
    "applicationSecurity", "recovery", "operations",
)


def build_release_gate(*, release_id: str, source_commit: str,
                       profile_digest: str,
                       dimensions: dict[str, dict[str, Any]],
                       execution_evidence: list[dict[str, Any]]) -> dict[str, Any]:
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
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    payload = json.loads(args.input.read_text(encoding="utf-8"))
    gate = build_release_gate(**payload)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(gate, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if gate["minindnCandidateOverall"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
