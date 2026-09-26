#!/usr/bin/env python3
"""Fail-closed completeness and hash-binding check before Tiger submission."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


SCHEMA = "ndnsf-itiger-pre-submit-checklist-v1"
GATES = {"control", "stage-readiness", "functional", "performance"}
COMMON_CHECKS = {
    "candidate-source-freshness",
    "candidate-closure-manifest",
    "candidate-invalidation-matrix",
    "proven-baseline-exact-delta",
    "release-identity",
    "local-sif-route",
    "cluster-substrate",
    "target-apptainer-parity",
    "container-abi-provenance",
    "complete-target-link-closure",
    "exact-sif-library-entrypoint",
    "submit-tree-helper-closure",
    "submit-env-contract",
    "bundle-cwd-artifact-mount",
    "isolated-home-pib-bootstrap",
    "controller-start-liveness",
    "lower-gates-native-exits",
    "wrapper-config-child-status",
    "resource-envelope",
    "result-boundary-label",
    "promotion-hash-config-delta",
    "credential-secret-scan",
    "predispatch-no-side-effects",
}
MODEL_CHECKS = {
    "current-sif-tiger-control",
    "onnx-model-runtime-compatibility",
    "onnx-native-session-probe",
    "cuda-no-fallback",
    "routes-stage-dataflow",
    "provider-pre-ready-lifecycle",
}
FUNCTIONAL_CHECKS = {
    "functional-bundle-identity-closure",
    "repository-service-route-readiness",
}


def required_checks(gate: str) -> set[str]:
    """Return the fail-closed row set for one submission gate.

    Stage readiness does not yet launch the Controller/User/Provider functional
    bundle, but every functional or performance submission does.  Keeping this
    distinction explicit prevents a prose-only functional-bundle requirement
    from silently disappearing from the machine gate.
    """
    required = set(COMMON_CHECKS)
    if gate != "control":
        required.update(MODEL_CHECKS)
    if gate in {"functional", "performance"}:
        required.update(FUNCTIONAL_CHECKS)
    return required


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize_digest(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    digest = value.lower()
    if digest.startswith("sha256:"):
        digest = digest[7:]
    if len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
        return None
    return digest


def resolve_file(raw: Any, base: Path, label: str, errors: list[str]) -> Path | None:
    if not isinstance(raw, str) or not raw.strip():
        errors.append(f"{label}: path must be a nonempty string")
        return None
    path = Path(raw).expanduser()
    if not path.is_absolute():
        path = base / path
    path = path.resolve()
    if not path.is_file():
        errors.append(f"{label}: missing regular file: {path}")
        return None
    if path.stat().st_size == 0:
        errors.append(f"{label}: evidence file is empty: {path}")
        return None
    return path


def validate_bound_file(
    record: Any, base: Path, label: str, errors: list[str]
) -> dict[str, Any] | None:
    if not isinstance(record, dict):
        errors.append(f"{label}: expected object with path and sha256")
        return None
    path = resolve_file(record.get("path"), base, label, errors)
    expected = normalize_digest(record.get("sha256"))
    if expected is None:
        errors.append(f"{label}: invalid or missing sha256")
        return None
    if path is None:
        return None
    actual = sha256_file(path)
    if actual != expected:
        errors.append(
            f"{label}: sha256 mismatch for {path}: expected {expected}, got {actual}"
        )
    return {"path": str(path), "size": path.stat().st_size, "sha256": actual}


def validate_manifest(manifest_path: Path, expected_gate: str) -> dict[str, Any]:
    errors: list[str] = []
    try:
        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {"status": "FAIL", "errors": [f"cannot read manifest: {exc}"]}

    if not isinstance(payload, dict):
        return {"status": "FAIL", "errors": ["manifest root must be an object"]}
    if payload.get("schema") != SCHEMA:
        errors.append(f"schema must equal {SCHEMA}")
    gate = payload.get("gate")
    if gate not in GATES:
        errors.append(f"gate must be one of {sorted(GATES)}")
    if gate != expected_gate:
        errors.append(f"manifest gate {gate!r} does not match --gate {expected_gate!r}")
    candidate_id = payload.get("candidateId")
    if not isinstance(candidate_id, str) or not candidate_id.strip():
        errors.append("candidateId must be a nonempty string")

    base = manifest_path.parent.resolve()
    sif_result = validate_bound_file(payload.get("sif"), base, "sif", errors)
    checks = payload.get("checks")
    if not isinstance(checks, dict):
        checks = {}
        errors.append("checks must be an object")

    required = required_checks(expected_gate)

    observed: dict[str, Any] = {}
    for check_id in sorted(required):
        row = checks.get(check_id)
        if not isinstance(row, dict):
            errors.append(f"{check_id}: missing required check object")
            continue
        if row.get("status") != "PASS":
            errors.append(f"{check_id}: status must be PASS")
        evidence = row.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            errors.append(f"{check_id}: evidence must be a nonempty list")
            continue
        bound: list[dict[str, Any]] = []
        for index, item in enumerate(evidence):
            result = validate_bound_file(
                item, base, f"{check_id}.evidence[{index}]", errors
            )
            if result is not None:
                bound.append(result)
        observed[check_id] = bound

    known_checks = COMMON_CHECKS | MODEL_CHECKS | FUNCTIONAL_CHECKS
    unexpected = sorted(set(checks) - known_checks)
    if unexpected:
        errors.append("unknown checklist row(s): " + ", ".join(unexpected))

    return {
        "schema": SCHEMA,
        "status": "PASS" if not errors else "FAIL",
        "gate": expected_gate,
        "candidateId": candidate_id,
        "manifest": str(manifest_path),
        "manifestSha256": sha256_file(manifest_path),
        "sif": sif_result,
        "requiredChecks": sorted(required),
        "observedEvidence": observed,
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--gate", required=True, choices=sorted(GATES))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    manifest_path = args.manifest.expanduser().resolve()
    result = validate_manifest(manifest_path, args.gate)
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
