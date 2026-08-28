#!/usr/bin/env python3
"""Admit immutable retained runtime evidence after a gate-analyzer failure."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "spec168_local_gate", HERE / "spec168_local_gate.py")
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            value.update(block)
    return "sha256:" + value.hexdigest()


def admit(
    evidence_dir: Path, output_dir: Path, *, expected_source_digest: str,
) -> dict:
    evidence_dir = evidence_dir.resolve()
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    result_path = output_dir / "gate-manifest.json"
    if result_path.exists():
        raise gate.GateError(
            "ANALYZER_DUPLICATE_RESULT_WRITER", "gate result already exists")
    runtime_path = evidence_dir / "runtime-admission.json"
    predecessor_path = evidence_dir / "gate-manifest.json"
    log_path = evidence_dir / "launcher.log"
    for path in (runtime_path, predecessor_path, log_path):
        if not path.is_file():
            raise gate.GateError(
                "RETAINED_EVIDENCE_MISSING", str(path))
    runtime = gate.load_object(runtime_path)
    if runtime.get("sourceDigest") != expected_source_digest:
        raise gate.GateError(
            "RETAINED_SOURCE_IDENTITY_MISMATCH",
            f"expected {expected_source_digest}, observed {runtime.get('sourceDigest')}",
        )
    predecessor = gate.load_object(predecessor_path)
    if predecessor.get("status") != "BLOCK":
        raise gate.GateError(
            "RETAINED_PREDECESSOR_NOT_BLOCKED", str(predecessor.get("status")))
    validation = gate.validate_runtime_manifest(
        evidence_dir, runtime, mode="real-minindn")
    result = {
        "schema": gate.GATE_SCHEMA,
        "gate": "B",
        "mode": "real-minindn",
        "status": "PASS",
        "failureCode": None,
        "failureMessage": None,
        "automaticRetry": False,
        "runtimeReexecuted": False,
        "retainedEvidence": True,
        "sourceDigest": expected_source_digest,
        "evidenceDirectory": str(evidence_dir),
        "runtimeManifest": str(runtime_path),
        "runtimeManifestDigest": digest(runtime_path),
        "launcherLog": str(log_path),
        "launcherLogDigest": digest(log_path),
        "predecessorGateManifest": str(predecessor_path),
        "predecessorGateManifestDigest": digest(predecessor_path),
        "predecessorFailureCode": predecessor.get("failureCode"),
        "validation": validation,
    }
    gate.atomic_write(result_path, result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--expected-source-digest", required=True)
    args = parser.parse_args()
    try:
        result = admit(
            args.evidence_dir,
            args.output_dir,
            expected_source_digest=args.expected_source_digest,
        )
    except gate.GateError as exc:
        print(json.dumps({
            "schema": gate.GATE_SCHEMA,
            "gate": "B",
            "mode": "real-minindn",
            "status": "BLOCK",
            "failureCode": exc.code,
            "failureMessage": str(exc),
            "automaticRetry": False,
            "runtimeReexecuted": False,
        }, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
