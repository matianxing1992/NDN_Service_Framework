#!/usr/bin/env python3
"""Runtime-aware NDNSF-DI planner contract tests."""

from __future__ import annotations

import unittest

from ndnsf_distributed_inference.runtime_v1 import (
    DiFragmentRuntimeState,
    DiLeaseResourceBinding,
    DiProviderRuntimeState,
    FragmentResidency,
    GenericAckMetadata,
    GenericAdmissionLease,
    GenericProviderRuntimeHint,
    ModelFragmentKey,
    PeerNetworkMetric,
    PlanDependency,
    PlanRole,
    PlanTemplate,
    ProviderNetworkMatrix,
    ReplanRecord,
    choose_bounded_replan_assignment,
    choose_edge_aware_runtime_assignment,
    choose_runtime_assignment,
    residency_ready_cost_ms,
    score_runtime_candidate,
)


def fragment(stage_index: int = 0, digest: str = "sha256:stage0") -> ModelFragmentKey:
    return ModelFragmentKey(
        model_id="qwen-tiny",
        model_digest="sha256:model",
        runtime_backend="onnx-cuda",
        precision="fp16",
        split_strategy="pipeline",
        stage_index=stage_index,
        stage_count=2,
        layer_start=stage_index * 14,
        layer_end=stage_index * 14 + 13,
        fragment_digest=digest,
    )


def candidate(provider: str,
              key: ModelFragmentKey,
              residency: FragmentResidency,
              *,
              queue: int = 0,
              ready_ms: float = 0.0,
              compute_ms: float = 10.0,
              lease_id: str = "") -> dict:
    lease = ()
    if lease_id:
        lease = (GenericAdmissionLease(
            lease_id=lease_id,
            request_id="req-1",
            service_name="/Inference/NativeTracer",
            provider_name=provider,
            expires_at_ms=4102444800000,
            resource_binding={"roleId": "/Stage/0"},
        ),)
    metadata = GenericAckMetadata(
        provider_runtime_hint=GenericProviderRuntimeHint(
            provider_name=provider,
            queue_length=queue,
            estimated_queue_wait_ms=queue * 10,
        ),
        lease_offers=lease,
        service_payload_schema="ndnsf-di-runtime-ack-v1",
        service_payload={
            "providerName": provider,
            "fragmentStates": [
                {
                    "fragmentKey": key,
                    "residency": residency.value,
                    "estimatedReadyMs": ready_ms,
                }
            ],
            "estimatedComputeMs": compute_ms,
        },
    )
    return {"providerName": provider, "genericAckMetadata": metadata}


class RuntimeAwarePlannerTest(unittest.TestCase):
    def test_fragment_and_di_state_round_trip(self) -> None:
        key = fragment()
        state = DiFragmentRuntimeState(
            fragment_key=key,
            residency=FragmentResidency.GPU_LOADED,
            estimated_ready_ms=0,
            memory_footprint_mb=1024,
        )
        parsed = DiProviderRuntimeState.from_dict({
            "providerName": "/provider/A",
            "fragmentStates": [state],
        })
        self.assertEqual(parsed.fragment_state_for(key).residency, FragmentResidency.GPU_LOADED)
        binding = DiLeaseResourceBinding.from_dict({
            "roleId": "/Stage/0",
            "fragmentKey": key,
            "residency": "GPU_LOADED",
        })
        self.assertTrue(binding.matches(role_id="/Stage/0", fragment_key=key))
        self.assertFalse(binding.matches(role_id="/Stage/1", fragment_key=key))

    def test_residency_cost_ordering(self) -> None:
        ordered = [
            residency_ready_cost_ms(FragmentResidency.GPU_LOADED),
            residency_ready_cost_ms(FragmentResidency.CPU_RESIDENT),
            residency_ready_cost_ms(FragmentResidency.DISK_RESIDENT),
            residency_ready_cost_ms(FragmentResidency.REPO_AVAILABLE),
            residency_ready_cost_ms(FragmentResidency.MISSING),
        ]
        self.assertEqual(ordered, sorted(ordered))

    def test_runtime_assignment_prefers_gpu_loaded_fragment(self) -> None:
        key = fragment()
        template = PlanTemplate(
            template_id="template-1",
            model_id="qwen-tiny",
            roles=(PlanRole("/Stage/0", key, estimated_compute_ms=10),),
        )
        assignment = choose_runtime_assignment(
            template,
            {
                "/Stage/0": [
                    candidate("/provider/disk", key, FragmentResidency.DISK_RESIDENT),
                    candidate("/provider/gpu", key, FragmentResidency.GPU_LOADED),
                ]
            },
            request_id="req-1",
        )
        self.assertEqual(assignment.role_assignments["/Stage/0"]["provider"], "/provider/gpu")
        self.assertEqual(assignment.role_assignments["/Stage/0"]["residency"], "GPU_LOADED")

    def test_required_runtime_state_excludes_missing_metadata(self) -> None:
        key = fragment()
        role = PlanRole("/Stage/0", key)
        missing = score_runtime_candidate(role, {"providerName": "/provider/legacy"}, runtime_required=True)
        fallback = score_runtime_candidate(role, {"providerName": "/provider/legacy"}, runtime_required=False)
        self.assertFalse(missing["valid"])
        self.assertEqual(missing["reason"], "RUNTIME_METADATA_REQUIRED")
        self.assertTrue(fallback["valid"])
        self.assertEqual(fallback["reason"], "CONSERVATIVE_FALLBACK")

    def test_network_matrix_penalizes_unknown_and_stale_edges(self) -> None:
        matrix = ProviderNetworkMatrix.from_dict({
            "staleAfterMs": 10,
            "stalePenaltyMs": 25,
            "unknownPenaltyMs": 100,
            "metrics": [
                {
                    "srcPeer": "/A",
                    "dstPeer": "/B",
                    "rttMs": 10,
                    "bandwidthMbps": 1000,
                    "updatedAtMs": 0,
                    "confidence": 1.0,
                }
            ],
        })
        stale, detail = matrix.transfer_cost_ms("/A", "/B", 1024, now_ms_value=100)
        unknown, unknown_detail = matrix.transfer_cost_ms("/B", "/A", 1024, now_ms_value=100)
        self.assertTrue(detail["stale"])
        self.assertTrue(unknown_detail["unknown"])
        self.assertGreater(unknown, stale)

    def test_edge_aware_assignment_avoids_bad_provider_pair(self) -> None:
        key0 = fragment(0, "sha256:stage0")
        key1 = fragment(1, "sha256:stage1")
        template = PlanTemplate(
            template_id="template-2",
            model_id="qwen-tiny",
            roles=(
                PlanRole("/Stage/0", key0, estimated_compute_ms=1),
                PlanRole("/Stage/1", key1, estimated_compute_ms=1),
            ),
            dependencies=(PlanDependency("/Stage/0", "/Stage/1", bytes_count=10_000_000),),
        )
        provider_candidates = {
            "/Stage/0": [
                candidate("/fast-a", key0, FragmentResidency.GPU_LOADED),
                candidate("/near-a", key0, FragmentResidency.CPU_RESIDENT),
            ],
            "/Stage/1": [
                candidate("/fast-b", key1, FragmentResidency.GPU_LOADED),
                candidate("/near-b", key1, FragmentResidency.CPU_RESIDENT),
            ],
        }
        matrix = ProviderNetworkMatrix(
            [
                PeerNetworkMetric("/fast-a", "/fast-b", rtt_ms=100, bandwidth_mbps=5, confidence=1.0),
                PeerNetworkMetric("/near-a", "/near-b", rtt_ms=2, bandwidth_mbps=1000, confidence=1.0),
            ],
            unknown_penalty_ms=10000,
        )
        assignment = choose_edge_aware_runtime_assignment(
            template,
            provider_candidates,
            request_id="req-1",
            network_matrix=matrix,
        )
        self.assertEqual(assignment.role_assignments["/Stage/0"]["provider"], "/near-a")
        self.assertEqual(assignment.role_assignments["/Stage/1"]["provider"], "/near-b")
        self.assertGreater(assignment.score_breakdown["edgeCostMs"], 0)

    def test_replan_record_serializes_reason(self) -> None:
        record = ReplanRecord(
            request_id="req-1",
            attempt=1,
            failed_provider="/provider/A",
            failed_lease_id="lease-1",
            reason_code="FRAGMENT_EVICTED",
            excluded_providers=("/provider/A",),
        )
        self.assertEqual(record.reason_code, "FRAGMENT_EVICTED")
        self.assertEqual(record.excluded_providers, ("/provider/A",))

    def test_bounded_replan_excludes_evicted_provider(self) -> None:
        key = fragment()
        template = PlanTemplate(
            template_id="template-replan",
            model_id="qwen-tiny",
            roles=(PlanRole("/Stage/0", key, estimated_compute_ms=10),),
        )
        providers = {
            "/Stage/0": [
                candidate("/provider/A", key, FragmentResidency.GPU_LOADED),
                candidate("/provider/B", key, FragmentResidency.CPU_RESIDENT),
            ],
        }
        initial = choose_bounded_replan_assignment(
            template,
            providers,
            request_id="req-replan",
            max_attempts=2,
        )
        self.assertEqual(initial.role_assignments["/Stage/0"]["provider"], "/provider/A")

        record = ReplanRecord.from_failure(
            request_id="req-replan",
            attempt=1,
            failed_provider="/provider/A",
            reason_code="FRAGMENT_EVICTED",
        )
        replanned = choose_bounded_replan_assignment(
            template,
            providers,
            request_id="req-replan",
            replan_records=(record,),
            max_attempts=2,
        )
        self.assertEqual(replanned.role_assignments["/Stage/0"]["provider"], "/provider/B")
        self.assertEqual(replanned.replan_attempt, 1)
        self.assertEqual(replanned.score_breakdown["excludedProviders"], ["/provider/A"])

    def test_bounded_replan_reports_max_attempts(self) -> None:
        key = fragment()
        template = PlanTemplate(
            template_id="template-replan-max",
            model_id="qwen-tiny",
            roles=(PlanRole("/Stage/0", key, estimated_compute_ms=10),),
        )
        providers = {
            "/Stage/0": [candidate("/provider/A", key, FragmentResidency.GPU_LOADED)],
        }
        record = ReplanRecord.from_failure(
            request_id="req-replan-max",
            attempt=1,
            failed_provider="/provider/A",
            reason_code="FRAGMENT_EVICTED",
        )
        with self.assertRaisesRegex(ValueError, "MAX_REPLAN_ATTEMPTS_EXCEEDED"):
            choose_bounded_replan_assignment(
                template,
                providers,
                request_id="req-replan-max",
                replan_records=(record,),
                max_attempts=1,
            )


if __name__ == "__main__":
    unittest.main()
