#!/usr/bin/env python3
"""Spec 107 release-input fail-closed contract tests."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from ndnsf_distributed_inference.runtime_v1_evidence import (  # noqa: E402
    SPEC107_RELEASE_DIMENSIONS,
    Spec107EvidenceBindingV1,
    evaluate_spec107_release_input,
)
from ndnsf_distributed_inference.release_gate import build_spec107_release_gate  # noqa: E402
from build_spec107_release_bundle import (  # noqa: E402
    ReleaseBundleError,
    build_release_bundle,
)


CANDIDATE_ID = (
    "spec107-c1-111111111111-222222222222-333333333333-"
    "444444444444-555555555555-666666666666")


class Spec107ReleaseInputTest(unittest.TestCase):
    def _fixture(self, root: Path) -> dict[str, object]:
        proof = root / "proof.json"
        proof.write_text(json.dumps({"status": "PASS"}), encoding="utf-8")
        digest = "sha256:" + hashlib.sha256(proof.read_bytes()).hexdigest()
        return {
            "schema": "ndnsf-di-spec107-release-input-v1",
            "candidateId": CANDIDATE_ID,
            "predecessor": {
                "releaseId": "spec105-local-minindn-candidate-r2",
                "minindnCandidateOverall": "BLOCK",
                "physicalProductionOverall": "DEFERRED",
            },
            "dimensions": {
                name: {"status": "PASS", "artifacts": ["proof.json"]}
                for name in SPEC107_RELEASE_DIMENSIONS
            },
            "evidenceManifest": [{
                "path": "proof.json",
                "sha256": digest,
                "candidateId": CANDIDATE_ID,
                "eligibility": "EVIDENCE_ELIGIBLE",
            }],
            "physicalProductionOverall": "DEFERRED",
        }

    def test_complete_digest_bound_input_is_eligible(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = evaluate_spec107_release_input(self._fixture(root), evidence_root=root)
            self.assertTrue(result["eligible"])
            self.assertEqual(result["errors"], [])
            self.assertEqual(result["physicalProductionOverall"], "DEFERRED")

    def test_missing_failed_and_tampered_evidence_block(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            missing = self._fixture(root)
            (root / "proof.json").unlink()
            result = evaluate_spec107_release_input(missing, evidence_root=root)
            self.assertIn("EVIDENCE_MISSING:proof.json", result["errors"])

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            failed = self._fixture(root)
            failed["dimensions"]["performance"]["status"] = "BLOCK"  # type: ignore[index]
            result = evaluate_spec107_release_input(failed, evidence_root=root)
            self.assertIn("DIMENSION_BLOCK:performance", result["errors"])

            tampered = self._fixture(root)
            (root / "proof.json").write_text("tampered\n", encoding="utf-8")
            result = evaluate_spec107_release_input(tampered, evidence_root=root)
            self.assertIn("EVIDENCE_DIGEST_MISMATCH:proof.json", result["errors"])

    def test_mixed_candidate_and_diagnostic_evidence_block(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mixed = self._fixture(root)
            mixed["evidenceManifest"][0]["candidateId"] = "spec107-c1-other"  # type: ignore[index]
            result = evaluate_spec107_release_input(mixed, evidence_root=root)
            self.assertIn("EVIDENCE_CANDIDATE_MISMATCH:proof.json", result["errors"])

            diagnostic = self._fixture(root)
            diagnostic["evidenceManifest"][0]["eligibility"] = "DIAGNOSTIC_INELIGIBLE"  # type: ignore[index]
            result = evaluate_spec107_release_input(diagnostic, evidence_root=root)
            self.assertIn("EVIDENCE_INELIGIBLE:proof.json", result["errors"])

    def test_physical_pass_and_wrong_predecessor_are_impossible(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            physical = self._fixture(root)
            physical["physicalProductionOverall"] = "PASS"
            result = evaluate_spec107_release_input(physical, evidence_root=root)
            self.assertIn("PHYSICAL_STATUS_MUST_BE_DEFERRED", result["errors"])
            self.assertEqual(result["physicalProductionOverall"], "DEFERRED")

            predecessor = self._fixture(root)
            predecessor["predecessor"]["minindnCandidateOverall"] = "PASS"  # type: ignore[index]
            result = evaluate_spec107_release_input(predecessor, evidence_root=root)
            self.assertIn("PREDECESSOR_BLOCK_NOT_PRESERVED", result["errors"])

    def test_unknown_missing_dimension_and_path_escape_block(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            payload = self._fixture(root)
            del payload["dimensions"]["recovery"]  # type: ignore[index]
            payload["dimensions"]["magic"] = {"status": "PASS", "artifacts": []}  # type: ignore[index]
            result = evaluate_spec107_release_input(payload, evidence_root=root)
            self.assertIn("DIMENSION_SET_INVALID", result["errors"])

            escaping = self._fixture(root)
            escaping["evidenceManifest"][0]["path"] = "../outside"  # type: ignore[index]
            result = evaluate_spec107_release_input(escaping, evidence_root=root)
            self.assertIn("EVIDENCE_PATH_INVALID:../outside", result["errors"])

    def test_candidate_evidence_binding_is_digest_bound_and_physical_deferred(self) -> None:
        payload = {
            "schema": "ndnsf-di-spec107-evidence-binding-v1",
            "candidateId": CANDIDATE_ID,
            "campaignId": "spec107-c1-performance-r1-aaaaaaaaaaaa",
            "campaignKind": "performance",
            "sourceDigest": "sha256:" + "1" * 64,
            "profileDigest": "sha256:" + "2" * 64,
            "modelDigest": "sha256:" + "3" * 64,
            "planDigest": "sha256:" + "4" * 64,
            "artifactDigest": "sha256:" + "5" * 64,
            "lineageDigest": "sha256:" + "6" * 64,
            "physicalProductionOverall": "DEFERRED",
        }
        value = Spec107EvidenceBindingV1.from_dict(payload)
        self.assertEqual(value.to_dict(), payload)
        payload["physicalProductionOverall"] = "PASS"
        with self.assertRaisesRegex(ValueError, "PHYSICAL_STATUS_MUST_BE_DEFERRED"):
            Spec107EvidenceBindingV1.from_dict(payload)

    def test_mechanical_gate_preserves_predecessor_block_and_physical_deferral(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            payload = self._fixture(root)
            gate = build_spec107_release_gate(payload, evidence_root=root)
            self.assertEqual(gate["minindnCandidateOverall"], "PASS")
            self.assertEqual(gate["predecessor"]["minindnCandidateOverall"], "BLOCK")
            self.assertEqual(gate["physicalProductionOverall"], "DEFERRED")
            payload["dimensions"]["performance"]["status"] = "BLOCK"  # type: ignore[index]
            blocked = build_spec107_release_gate(payload, evidence_root=root)
            self.assertEqual(blocked["minindnCandidateOverall"], "BLOCK")
            self.assertIn("DIMENSION_BLOCK:performance", blocked["errors"])

    def test_bundle_scans_content_before_exclusive_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            feature = root / "specs/107-feature"
            feature.mkdir(parents=True)
            payload = self._fixture(feature)
            (feature / "release-input.json").write_text(
                json.dumps(payload), encoding="utf-8")
            output = feature / "release-gate.json"
            gate = build_release_bundle(
                feature=feature, output=output, repo_root=root)
            self.assertEqual(gate["minindnCandidateOverall"], "PASS")
            self.assertTrue(output.is_file())

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            feature = root / "specs/107-feature"
            feature.mkdir(parents=True)
            payload = self._fixture(feature)
            (feature / "proof.json").write_text(
                json.dumps({"status": "PASS", "payload": "secret-content"}),
                encoding="utf-8")
            payload["evidenceManifest"][0]["sha256"] = (  # type: ignore[index]
                "sha256:" + hashlib.sha256(
                    (feature / "proof.json").read_bytes()).hexdigest())
            (feature / "release-input.json").write_text(
                json.dumps(payload), encoding="utf-8")
            output = feature / "release-gate.json"
            with self.assertRaisesRegex(ReleaseBundleError, "FORBIDDEN_EVIDENCE_CONTENT"):
                build_release_bundle(feature=feature, output=output, repo_root=root)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
