#!/usr/bin/env python3
"""Spec 105 deployment-readiness contract tests."""

from __future__ import annotations

import unittest

from ndnsf_distributed_inference.runtime_v1 import (
    ExecutionEvidenceV1,
    classify_execution_evidence,
)


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


class DeploymentReadinessContractsTest(unittest.TestCase):
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
