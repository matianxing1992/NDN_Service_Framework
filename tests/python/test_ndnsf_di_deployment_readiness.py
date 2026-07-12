#!/usr/bin/env python3
"""Spec 105 deployment-readiness contract tests."""

from __future__ import annotations

import threading
import unittest

from ndnsf_distributed_inference.runtime_v1 import (
    ExecutionEvidenceV1,
    classify_execution_evidence,
)
from ndnsf_distributed_inference.release_gate import DIMENSIONS, build_release_gate
from ndnsf_distributed_inference.qwen_pilot import (
    BoundedGenerationScheduler,
    CacheResolution,
    GenerationQueueFull,
    QwenPilotRequest,
    QwenPilotOrchestrator,
    QwenPilotTerminalError,
    compare_token_sequences,
    greedy_decode_fixture,
    resolve_cache_request,
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
    def test_generation_scheduler_owns_full_generation_before_next_job(self) -> None:
        observed: list[tuple[str, int]] = []
        scheduler = BoundedGenerationScheduler(max_workers=1, max_queued=1)

        def generation(session_id: str):
            def run(report_progress):
                for token_count in range(1, 4):
                    observed.append((session_id, token_count))
                    report_progress(token_count)
                return session_id
            return run

        first = scheduler.submit("generation-0", generation("generation-0"))
        second = scheduler.submit("generation-1", generation("generation-1"))
        self.assertEqual(first.result(timeout=1), "generation-0")
        self.assertEqual(second.result(timeout=1), "generation-1")
        scheduler.shutdown()

        self.assertEqual(observed, [
            ("generation-0", 1),
            ("generation-0", 2),
            ("generation-0", 3),
            ("generation-1", 1),
            ("generation-1", 2),
            ("generation-1", 3),
        ])

    def test_generation_scheduler_bounds_queue_and_reports_occupancy(self) -> None:
        started = threading.Event()
        release = threading.Event()
        start_lock = threading.Lock()
        start_count = 0
        scheduler = BoundedGenerationScheduler(max_workers=2, max_queued=2)

        def blocked(report_progress):
            nonlocal start_count
            report_progress(1)
            with start_lock:
                start_count += 1
                if start_count == 2:
                    started.set()
            release.wait(timeout=1)
            return "ok"

        futures = [
            scheduler.submit(f"generation-{index}", blocked)
            for index in range(4)
        ]
        self.assertTrue(started.wait(timeout=1))
        snapshot = scheduler.snapshot()
        self.assertEqual(snapshot.active, 2)
        self.assertEqual(snapshot.queued, 2)
        self.assertEqual(snapshot.max_active_observed, 2)
        self.assertEqual(snapshot.max_queued_observed, 2)
        self.assertEqual(snapshot.token_progress["generation-0"], 1)
        self.assertEqual(snapshot.token_progress["generation-1"], 1)
        with self.assertRaises(GenerationQueueFull):
            scheduler.submit("generation-overflow", blocked)

        release.set()
        self.assertEqual([future.result(timeout=1) for future in futures], ["ok"] * 4)
        scheduler.shutdown()

    def test_generation_scheduler_rejects_progress_regression_and_reports_terminals(self) -> None:
        scheduler = BoundedGenerationScheduler(max_workers=1, max_queued=1)

        def regressing(report_progress):
            report_progress(2)
            report_progress(1)

        failed = scheduler.submit("generation-failed", regressing)
        with self.assertRaisesRegex(ValueError, "must increase"):
            failed.result(timeout=1)
        completed = scheduler.submit(
            "generation-complete",
            lambda report_progress: report_progress(3) or "ok",
        )
        self.assertEqual(completed.result(timeout=1), "ok")
        snapshot = scheduler.snapshot()
        scheduler.shutdown()

        self.assertEqual(snapshot.completed, 1)
        self.assertEqual(snapshot.failed, 1)
        self.assertEqual(snapshot.unfinished, 0)
        self.assertEqual(snapshot.token_progress, {
            "generation-failed": 2,
            "generation-complete": 3,
        })

    def test_qwen_pilot_greedy_token_fixtures_1_2_and_32(self) -> None:
        logits = [[-1.0, float(index), 100.0 + index] for index in range(32)]
        self.assertEqual(greedy_decode_fixture(logits, 1), [2])
        self.assertEqual(greedy_decode_fixture(logits, 2), [2, 2])
        self.assertEqual(greedy_decode_fixture(logits, 32), [2] * 32)

    def test_qwen_pilot_admission_enforces_input_and_output_bounds(self) -> None:
        QwenPilotRequest(tuple(range(512)), 32).validate()
        with self.assertRaises(ValueError):
            QwenPilotRequest(tuple(range(513)), 1).validate()
        with self.assertRaises(ValueError):
            QwenPilotRequest((1,), 33).validate()
        with self.assertRaises(ValueError):
            QwenPilotRequest((), 1).validate()

    def test_qwen_pilot_cache_hit_rebuild_and_delta_only_failure(self) -> None:
        self.assertEqual(
            resolve_cache_request(cache_present=True, full_context_present=False,
                                  delta_only=True),
            CacheResolution.HIT,
        )
        self.assertEqual(
            resolve_cache_request(cache_present=False, full_context_present=True,
                                  delta_only=False),
            CacheResolution.FULL_CONTEXT_REBUILD,
        )
        with self.assertRaises(QwenPilotTerminalError) as caught:
            resolve_cache_request(cache_present=False, full_context_present=False,
                                  delta_only=True)
        self.assertEqual(caught.exception.reason,
                         "CACHE_MISS_FULL_CONTEXT_REQUIRED")

    def test_qwen_pilot_tokenization_orchestration_and_exact_comparison(self) -> None:
        contexts: list[tuple[int, ...]] = []
        orchestrator = QwenPilotOrchestrator(
            tokenizer=lambda prompt: [len(prompt), 7],
            staged_logits=lambda context: (
                contexts.append(context) or [0.0, 1.0, float(len(context))]
            ),
        )
        request = orchestrator.request("pilot", 2)
        actual = orchestrator.generate(request)
        self.assertEqual(actual, [2, 2])
        self.assertEqual(contexts, [(5, 7), (5, 7, 2)])
        compare_token_sequences([2, 2], actual)
        with self.assertRaises(QwenPilotTerminalError) as mismatch:
            compare_token_sequences([2, 1], actual)
        self.assertIn("index=1", mismatch.exception.reason)

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
    GenerationQueueFull,
