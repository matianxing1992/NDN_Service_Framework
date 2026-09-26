#!/usr/bin/env python3
"""Focused self-tests for the operator-side Tiger checklist validator."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


VALIDATOR_PATH = Path(__file__).with_name("validate-pre-tiger-checklist.py")
SPEC = importlib.util.spec_from_file_location("pre_tiger_validator", VALIDATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def subject(root: Path, gate: str) -> Path:
    sif = root / "runtime.sif"
    sif.write_bytes(b"candidate-sif")
    evidence = root / "evidence.json"
    evidence.write_text('{"status":"PASS"}\n', encoding="utf-8")
    checks = {
        check_id: {
            "status": "PASS",
            "evidence": [{"path": str(evidence), "sha256": digest(evidence)}],
        }
        for check_id in VALIDATOR.required_checks(gate)
    }
    manifest = root / "pre-tiger-checklist.json"
    manifest.write_text(
        json.dumps({
            "schema": VALIDATOR.SCHEMA,
            "gate": gate,
            "candidateId": "candidate-test",
            "sif": {"path": str(sif), "sha256": digest(sif)},
            "checks": checks,
        }),
        encoding="utf-8",
    )
    return manifest


class ChecklistGateTests(unittest.TestCase):
    def test_control_requires_new_common_incident_rows(self) -> None:
        required = VALIDATOR.required_checks("control")
        self.assertIn("candidate-source-freshness", required)
        self.assertIn("candidate-closure-manifest", required)
        self.assertIn("candidate-invalidation-matrix", required)
        self.assertIn("proven-baseline-exact-delta", required)
        self.assertIn("submit-tree-helper-closure", required)
        self.assertIn("submit-env-contract", required)
        self.assertIn("controller-start-liveness", required)
        self.assertIn("result-boundary-label", required)
        self.assertIn("predispatch-no-side-effects", required)

    def test_control_rejects_missing_proven_baseline_delta(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = subject(Path(directory), "control")
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            del payload["checks"]["proven-baseline-exact-delta"]
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            result = VALIDATOR.validate_manifest(manifest, "control")
        self.assertEqual("FAIL", result["status"])
        self.assertTrue(any(
            "proven-baseline-exact-delta" in error
            for error in result["errors"]
        ))

    def test_stage_readiness_requires_model_but_not_functional_bundle(self) -> None:
        required = VALIDATOR.required_checks("stage-readiness")
        self.assertIn("provider-pre-ready-lifecycle", required)
        self.assertIn("onnx-native-session-probe", required)
        self.assertNotIn("functional-bundle-identity-closure", required)

    def test_functional_requires_functional_bundle_identity_closure(self) -> None:
        required = VALIDATOR.required_checks("functional")
        self.assertIn("functional-bundle-identity-closure", required)
        self.assertIn("repository-service-route-readiness", required)
        with tempfile.TemporaryDirectory() as directory:
            manifest = subject(Path(directory), "functional")
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            del payload["checks"]["functional-bundle-identity-closure"]
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            result = VALIDATOR.validate_manifest(manifest, "functional")
        self.assertEqual("FAIL", result["status"])
        self.assertTrue(any(
            "functional-bundle-identity-closure" in error
            for error in result["errors"]
        ))

    def test_functional_rejects_missing_repository_service_readiness(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = subject(Path(directory), "functional")
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            del payload["checks"]["repository-service-route-readiness"]
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            result = VALIDATOR.validate_manifest(manifest, "functional")
        self.assertEqual("FAIL", result["status"])
        self.assertTrue(any(
            "repository-service-route-readiness" in error
            for error in result["errors"]
        ))

    def test_complete_gate_manifests_pass(self) -> None:
        for gate in sorted(VALIDATOR.GATES):
            with self.subTest(gate=gate), tempfile.TemporaryDirectory() as directory:
                manifest = subject(Path(directory), gate)
                result = VALIDATOR.validate_manifest(manifest, gate)
                self.assertEqual("PASS", result["status"], result["errors"])


if __name__ == "__main__":
    unittest.main()
