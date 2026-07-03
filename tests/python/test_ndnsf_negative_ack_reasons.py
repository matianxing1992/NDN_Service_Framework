#!/usr/bin/env python3
"""Negative ACK reason-code contract tests."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from ndnsf import (
    NEGATIVE_ACK_REASON_GPU_BUSY,
    NEGATIVE_ACK_REASON_INTERNAL_ERROR,
    NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
    NEGATIVE_ACK_REASON_PROVIDER_BUSY,
    NEGATIVE_ACK_REASON_QUEUE_FULL,
    RECOMMENDED_NEGATIVE_ACK_REASONS,
)
from ndnsf_distributed_inference.artifact_deployment import ArtifactProvisioningState
from ndnsf_distributed_inference.provider import (
    DistributedInferenceProvider,
    ProviderAdmissionPolicy,
)
from ndnsf_distributed_inference.runtime_v1 import RuntimeTelemetryV1

from Experiments.NDNSF_DI_NativeTracer_Minindn import (
    build_failure_breakdown,
    collect_negative_ack_reason_counters,
)


class _FakeProvider:
    def __init__(self) -> None:
        self.ack = None

    def add_collaboration_handler(self, _service, _roles, _handler, ack) -> None:
        self.ack = ack


class NegativeAckReasonContractTest(unittest.TestCase):
    def test_artifact_readiness_uses_recommended_reason_codes(self) -> None:
        state = ArtifactProvisioningState(
            component="qwen-stage",
            initial_status="installing",
            initial_message="downloading model",
        )
        installing = state.ack()
        self.assertFalse(installing.status)
        self.assertEqual(installing.message, NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE)
        self.assertIn(b"negativeAckReason=MODEL_UNAVAILABLE;", installing.payload)

        state.mark_failed("runtime crashed")
        failed = state.ack()
        self.assertFalse(failed.status)
        self.assertEqual(failed.message, NEGATIVE_ACK_REASON_INTERNAL_ERROR)
        self.assertIn(b"runtimeMessage=runtime crashed;", failed.payload)

        state.mark_ready("runtime ready")
        ready = state.ack()
        self.assertTrue(ready.status)
        self.assertNotIn(b"negativeAckReason=", ready.payload)

    def test_recommended_reason_set_contains_provider_busy(self) -> None:
        self.assertIn(NEGATIVE_ACK_REASON_PROVIDER_BUSY, RECOMMENDED_NEGATIVE_ACK_REASONS)

    def test_di_capability_rejects_unavailable_model_with_reason_code(self) -> None:
        fake = _FakeProvider()
        provider = DistributedInferenceProvider(fake)
        provider.add_capability_handler(
            "/Inference/Test",
            ["/Stage/0"],
            lambda _ctx: None,
            has_model=False,
            can_provision=False,
        )
        self.assertIsNotNone(fake.ack)
        decision = fake.ack(b"request")
        self.assertFalse(decision.status)
        self.assertEqual(decision.message, NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE)
        self.assertIn(b"negativeAckReason=MODEL_UNAVAILABLE", decision.payload)
        self.assertIn(b"status=model-unavailable", decision.payload)

    def test_di_capability_admission_policy_is_opt_in(self) -> None:
        fake = _FakeProvider()
        provider = DistributedInferenceProvider(fake)
        provider.add_capability_handler(
            "/Inference/Test",
            ["/Stage/0"],
            lambda _ctx: None,
            has_model=True,
            can_provision=False,
            runtime_telemetry=RuntimeTelemetryV1(
                provider="p0",
                runtime_backend="onnxruntime",
                ready_queue=99,
                active_workers=4,
            ),
        )
        decision = fake.ack(b"request")
        self.assertTrue(decision.status)
        self.assertIn(b"queue=103", decision.payload)

    def test_di_capability_admission_policy_rejects_queue_full(self) -> None:
        fake = _FakeProvider()
        provider = DistributedInferenceProvider(fake)
        provider.add_capability_handler(
            "/Inference/Test",
            ["/Stage/0"],
            lambda _ctx: None,
            has_model=True,
            can_provision=False,
            runtime_telemetry=RuntimeTelemetryV1(
                provider="p0",
                runtime_backend="onnxruntime",
                ready_queue=3,
                waiting_dependencies=1,
                active_workers=1,
            ),
            admission_policy=ProviderAdmissionPolicy(max_queue=5),
        )
        decision = fake.ack(b"request")
        self.assertFalse(decision.status)
        self.assertEqual(decision.message, NEGATIVE_ACK_REASON_QUEUE_FULL)
        self.assertIn(b"negativeAckReason=QUEUE_FULL", decision.payload)
        self.assertIn(b"admissionLimit=queue", decision.payload)

    def test_di_capability_admission_policy_rejects_gpu_busy(self) -> None:
        fake = _FakeProvider()
        provider = DistributedInferenceProvider(fake)
        provider.add_capability_handler(
            "/Inference/Test",
            ["/Stage/0"],
            lambda _ctx: None,
            has_model=True,
            can_provision=False,
            runtime_telemetry=RuntimeTelemetryV1(
                provider="p0",
                runtime_backend="onnxruntime",
                free_memory_mb=512,
                model_loaded=True,
            ),
            admission_policy=ProviderAdmissionPolicy(min_free_memory_mb=1024),
        )
        decision = fake.ack(b"request")
        self.assertFalse(decision.status)
        self.assertEqual(decision.message, NEGATIVE_ACK_REASON_GPU_BUSY)
        self.assertIn(b"negativeAckReason=GPU_BUSY", decision.payload)
        self.assertIn(b"admissionLimit=freeMemoryMb", decision.payload)

    def test_native_tracer_summary_counts_negative_ack_reasons(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            logs_dir = Path(tmp)
            (logs_dir / "user.log").write_text(
                "event=NEGATIVE_ACK_RECORDED providerName=/p/A reason=QUEUE_FULL\n"
                "event=NEGATIVE_ACK_RECORDED providerName=/p/B reason=MODEL_UNAVAILABLE\n",
                encoding="utf-8",
            )
            (logs_dir / "provider.log").write_text(
                "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION roles=/Backbone status=0 "
                "message=\"MODEL_UNAVAILABLE\"\n"
                "payload negativeAckReason=MODEL_UNAVAILABLE;\n",
                encoding="utf-8",
            )

            counters = collect_negative_ack_reason_counters(logs_dir)
            self.assertEqual(counters["userRecorded"], {
                "MODEL_UNAVAILABLE": 1,
                "QUEUE_FULL": 1,
            })
            self.assertEqual(counters["providerDecisions"], {
                "MODEL_UNAVAILABLE": 1,
            })
            self.assertEqual(counters["payloadReasons"], {
                "MODEL_UNAVAILABLE": 1,
            })

    def test_failure_breakdown_separates_timeout_and_negative_ack_signals(self) -> None:
        breakdown = build_failure_breakdown(
            {
                "requestCount": 2,
                "successCount": 1,
                "failureCount": 1,
                "requests": [
                    {"status": "executed"},
                    {"status": "failed", "error": "timeout: /request/1"},
                ],
            },
            {
                "userRecorded": {"QUEUE_FULL": 2},
                "providerDecisions": {"QUEUE_FULL": 2},
                "payloadReasons": {"QUEUE_FULL": 2},
            },
        )
        self.assertEqual(breakdown["timeoutCount"], 1)
        self.assertEqual(breakdown["negativeAckEventCount"], 2)
        self.assertEqual(breakdown["negativeAckReasons"], {"QUEUE_FULL": 2})


if __name__ == "__main__":
    unittest.main()
