#!/usr/bin/env python3
"""Seal one completed Spec175 Tiger gate as a cumulative prerequisite."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


SCHEMA = "ndnsf-di-spec175-gate-prerequisite-v1"
GATES = {"G4T", "G5", "G6", "G6C"}


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate", required=True, choices=sorted(GATES))
    parser.add_argument("--candidate-closure", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    closure_path = args.candidate_closure.expanduser().resolve()
    evidence_path = args.evidence.expanduser().resolve()
    closure = json.loads(closure_path.read_text(encoding="utf-8"))
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    candidate_tuple = closure.get("candidateTuple", {})
    if (closure.get("schema") != "ndnsf-di-spec175-candidate-closure-v1"
            or closure.get("status") not in {"PASS", "PROMOTABLE"}
            or closure.get("selectedGate") != args.gate
            or not isinstance(candidate_tuple, dict)):
        raise SystemExit("candidate closure is not a PASS for the sealed gate")
    evidence_status = evidence.get("status")
    if evidence_status != "PASS":
        raise SystemExit("gate evidence status is not PASS")
    identities = {
        "sourceSealSha256": candidate_tuple.get("sourceSeal"),
        "sifSha256": candidate_tuple.get("exactSif"),
        "workloadSha256": candidate_tuple.get("workloadConfig"),
        "modelManifestSha256": candidate_tuple.get("modelArtifacts"),
    }
    if any(not isinstance(value, str) or not value.startswith("sha256:")
           or len(value) != 71 for value in identities.values()):
        raise SystemExit("candidate closure identity is incomplete")
    document = {
        "schemaVersion": SCHEMA, "status": "PASS", "gate": args.gate,
        "candidateId": closure.get("candidateId"),
        **identities,
        "candidateClosurePath": str(closure_path),
        "candidateClosureSha256": sha256(closure_path),
        "evidencePath": str(evidence_path),
        "evidenceSha256": sha256(evidence_path),
    }
    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(json.dumps({
        "status": "PASS", "gate": args.gate,
        "output": str(output), "sha256": sha256(output)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
