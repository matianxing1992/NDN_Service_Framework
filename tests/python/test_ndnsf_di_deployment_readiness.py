#!/usr/bin/env python3
"""Spec 105 deployment-readiness contract tests."""

from __future__ import annotations

import unittest

from ndnsf_distributed_inference.runtime_v1 import (
    ExecutionEvidenceV1,
    classify_execution_evidence,
)
from ndnsf_distributed_inference.release_gate import DIMENSIONS, build_release_gate


def evidence(kind: str, *, real: bool, artifact: str = "sha256:a") -> ExecutionEvidenceV1:
    return ExecutionEvidenceV1.from_dict({
        "schema": "ndnsf-di-execution-evidence-v1",
        "providerName": "/provider/A",
        "providerBootId": "boot-a",
        "evidenceEpoch": 1,
        "runnerKind": kind,
        "realCompute": real,
        "device": {"kind": "cuda" if "cuda" in kind else "cpu",
                   "id": "GPU-1" if "cuda" in kind else "cpu0"},
        "runtimeVersion": "runtime-v1",
        "modelDigest": "sha256:model",
        "planDigest": "sha256:plan",
        "artifactDigests": {"/LLM/Stage/0": artifact},
        "roles": ["/LLM/Stage/0"],
        "createdAtMs": 1,
    })


def evidence_payload(value: ExecutionEvidenceV1) -> dict[str, object]:
    return {
        "schema": value.schema,
        "providerName": value.provider_name,
        "providerBootId": value.provider_boot_id,
        "evidenceEpoch": value.evidence_epoch,
        "runnerKind": value.runner_kind.value,
        "realCompute": value.real_compute,
        "device": {"kind": value.device_kind, "id": value.device_id},
        "runtimeVersion": value.runtime_version,
        "modelDigest": value.model_digest,
        "planDigest": value.plan_digest,
        "artifactDigests": value.artifact_digests,
        "roles": list(value.roles),
        "createdAtMs": value.created_at_ms,
    }


class DeploymentReadinessContractsTest(unittest.TestCase):
    def test_release_gate_blocks_missing_synthetic_mixed_and_digest_mismatch(self) -> None:
        dimensions = {name: {"status": "PASS", "artifacts": [f"{name}.json"]}
                      for name in DIMENSIONS}
        real = evidence("onnxruntime-cuda", real=True)
        passing = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions,
            execution_evidence=[evidence_payload(real)])
        self.assertEqual(passing["minindnCandidateOverall"], "PASS")
        self.assertEqual(passing["physicalProductionOverall"], "DEFERRED")
        missing = build_release_gate(release_id="r", source_commit="c",
                                     profile_digest="sha256:p", dimensions=dimensions,
                                     execution_evidence=[])
        self.assertEqual(missing["minindnCandidateOverall"], "BLOCK")
        synthetic = evidence("synthetic-delay", real=False)
        blocked = build_release_gate(release_id="r", source_commit="c",
                                     profile_digest="sha256:p", dimensions=dimensions,
                                     execution_evidence=[evidence_payload(synthetic)])
        self.assertEqual(blocked["minindnCandidateOverall"], "BLOCK")

        mixed = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions,
            execution_evidence=[
                evidence_payload(evidence("onnxruntime-cpu", real=True)),
                evidence_payload(evidence("onnxruntime-cuda", real=True)),
            ],
        )
        self.assertEqual(mixed["minindnCandidateOverall"], "BLOCK")

        digest_mismatch = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions,
            execution_evidence=[
                evidence_payload(evidence("onnxruntime-cuda", real=True,
                                          artifact="sha256:a")),
                evidence_payload(evidence("onnxruntime-cuda", real=True,
                                          artifact="sha256:b")),
            ],
        )
        self.assertEqual(digest_mismatch["minindnCandidateOverall"], "BLOCK")

        contradictory = evidence_payload(evidence("synthetic-delay", real=False))
        contradictory["realCompute"] = True
        contradiction_gate = build_release_gate(
            release_id="r", source_commit="c", profile_digest="sha256:p",
            dimensions=dimensions, execution_evidence=[contradictory],
        )
        self.assertEqual(contradiction_gate["minindnCandidateOverall"], "BLOCK")

    def test_release_gate_blocks_missing_dimension_artifact(self) -> None:
        dimensions = {name: {"status": "PASS", "artifacts": ["proof"]}
                      for name in DIMENSIONS}
        dimensions["operations"] = {"status": "PASS", "artifacts": []}
        gate = build_release_gate(release_id="r", source_commit="c",
                                  profile_digest="sha256:p", dimensions=dimensions,
                                  execution_evidence=[])
        self.assertEqual(gate["dimensions"]["operations"]["status"], "BLOCK")
        self.assertEqual(gate["minindnCandidateOverall"], "BLOCK")

    def test_synthetic_wiring_cpu_and_cuda_classify_distinctly(self) -> None:
        self.assertEqual(classify_execution_evidence([evidence("synthetic-delay", real=False)]),
                         "synthetic-delay")
        self.assertEqual(classify_execution_evidence([evidence("wiring-only", real=False)]),
                         "wiring-only")
        self.assertEqual(classify_execution_evidence([evidence("onnxruntime-cpu", real=True)]),
                         "onnxruntime-cpu")
        self.assertEqual(classify_execution_evidence([evidence("onnxruntime-cuda", real=True)]),
                         "onnxruntime-cuda")

    def test_missing_mixed_and_contradictory_evidence_fail(self) -> None:
        self.assertEqual(classify_execution_evidence([]), "invalid-evidence")
        self.assertEqual(classify_execution_evidence([
            evidence("onnxruntime-cpu", real=True),
            evidence("onnxruntime-cuda", real=True),
        ]), "invalid-evidence")
        with self.assertRaises(ValueError):
            evidence("synthetic-delay", real=True)

    def test_artifact_mismatch_is_not_aggregated(self) -> None:
        first = evidence("onnxruntime-cuda", real=True, artifact="sha256:a")
        second = evidence("onnxruntime-cuda", real=True, artifact="sha256:b")
        self.assertEqual(classify_execution_evidence([first, second]), "invalid-evidence")


if __name__ == "__main__":
    unittest.main()
