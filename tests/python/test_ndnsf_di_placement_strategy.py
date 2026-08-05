from __future__ import annotations

from dataclasses import replace
import unittest
from pathlib import Path
import socket
import time

from ndnsf_distributed_inference.core.decision_validation import (
    reject_placement_sensitive,
)
from ndnsf_distributed_inference.core.contracts import (
    DATA_DRIVEN_V2, LEGACY_READY_SET_V1,
)
from ndnsf_distributed_inference.core.ports import CandidateBudget
from ndnsf_distributed_inference.sdk.placement import (
    ArtifactPreparationMode,
    DIProviderOfferV2,
    ModelPlacementStrategy,
    PlacementDecision,
    PlacementRequest,
    ProviderAssignment,
    ProviderPlanningView,
    build_provider_planning_view,
    evaluate_placement_strategy,
)


class _DeterministicStrategy(ModelPlacementStrategy):
    name = "deterministic-test"
    version = "1"
    state_digest = "sha256:" + "a" * 64

    def plan(self, request: PlacementRequest) -> PlacementDecision:
        return PlacementDecision(
            split_id="split-a",
            split_digest="sha256:" + "b" * 64,
            assignments=(
                ProviderAssignment(
                    role="stage-0",
                    provider="/provider/a",
                    required_gpu_memory_mb=1024,
                    backend="onnxruntime",
                ),
            ),
            fallback_order={"stage-0": ("/provider/a",)},
            input_digest=request.digest(),
            evidence_digest="sha256:" + "c" * 64,
        )


class PlacementStrategyContractTest(unittest.TestCase):
    def test_canonical_request_digest_and_deterministic_replay(self):
        request = PlacementRequest(
            request_id="request-a",
            attempt=1,
            deadline_ms=2_000_000_000_000,
            model_digest="sha256:" + "d" * 64,
            graph_digest="sha256:" + "e" * 64,
            candidate_ids=("split-a",),
            providers=(),
            required_roles=("stage-0",),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
        )

        self.assertEqual(request.digest(), request.digest())
        decision = evaluate_placement_strategy(
            _DeterministicStrategy(), request, replay_deterministic=True)
        self.assertEqual(decision.input_digest, request.digest())

    def test_positive_signed_offer_becomes_sanitized_planning_view(self):
        offer = DIProviderOfferV2(
            profile="ndnsf-di-provider-offer-v2",
            profile_version=2,
            request_id="request-a",
            attempt=1,
            service="/inference",
            provider="/provider/a",
            model_intent_digest="sha256:" + "d" * 64,
            boot_epoch="boot-epoch-0001",
            resource_sequence=7,
            captured_at_ms=1000,
            expires_at_ms=5000,
            accepted_deadline_ms=4500,
            accepted_roles=("stage-0", "stage-1"),
            backends=("onnxruntime",),
            devices=("cuda:0",),
            offered_gpu_memory_mb=12_288,
            queue_depth=1,
            estimated_wait_ms=2.5,
            rtt_ms=5.0,
            bandwidth_mbps=1000.0,
            capability_resource_digest="sha256:" + "e" * 64,
            acceptance_predicate_digest="sha256:" + "f" * 64,
            evidence_digest="sha256:" + "1" * 64,
            signer_key_id="/provider/a/KEY/1",
            signature="valid-signature",
            cached_shards=({"artifact_digest": "sha256:" + "2" * 64},),
        )

        view = build_provider_planning_view(
            offer,
            ack_status=True,
            at_ms=2000,
            request_id="request-a",
            attempt=1,
            model_intent_digest="sha256:" + "d" * 64,
            deadline_ms=4500,
            verify_signature=lambda value: value.signature == "valid-signature",
        )

        self.assertEqual(view.usable_gpu_memory_mb, 12_288)
        self.assertEqual(view.offer_digest, offer.digest())
        self.assertEqual(view.execution_policies, (DATA_DRIVEN_V2,))
        self.assertNotIn("signature", view.to_dict())
        self.assertNotIn("token", repr(view.to_dict()).lower())
        with self.assertRaises(TypeError):
            view.cached_shards[0]["artifact_digest"] = "changed"
        with self.assertRaisesRegex(ValueError, "execution policy"):
            build_provider_planning_view(
                replace(offer, execution_policies=(LEGACY_READY_SET_V1,)),
                ack_status=True, at_ms=2000, request_id="request-a",
                attempt=1,
                model_intent_digest="sha256:" + "d" * 64,
                deadline_ms=4500,
                verify_signature=lambda _value: True,
            )

    def test_invalid_ack_offers_fail_closed(self):
        def offer(**changes):
            values = dict(
                profile="ndnsf-di-provider-offer-v2",
                profile_version=2,
                request_id="request-a",
                attempt=1,
                service="/inference",
                provider="/provider/a",
                model_intent_digest="sha256:" + "d" * 64,
                boot_epoch="boot-epoch-0001",
                resource_sequence=7,
                captured_at_ms=1000,
                expires_at_ms=5000,
                accepted_deadline_ms=4500,
                accepted_roles=("stage-0",),
                backends=("onnxruntime",),
                devices=("cuda:0",),
                offered_gpu_memory_mb=4096,
                queue_depth=0,
                estimated_wait_ms=0.0,
                rtt_ms=1.0,
                bandwidth_mbps=1000.0,
                capability_resource_digest="sha256:" + "e" * 64,
                acceptance_predicate_digest="sha256:" + "f" * 64,
                evidence_digest="sha256:" + "1" * 64,
                signer_key_id="/provider/a/KEY/1",
                signature="valid",
            )
            values.update(changes)
            return DIProviderOfferV2(**values)

        common = dict(
            at_ms=2000,
            request_id="request-a",
            attempt=1,
            model_intent_digest="sha256:" + "d" * 64,
            deadline_ms=4500,
            verify_signature=lambda value: value.signature == "valid",
        )
        with self.assertRaisesRegex(ValueError, "negative ACK"):
            build_provider_planning_view(offer(), ack_status=False, **common)
        with self.assertRaisesRegex(ValueError, "signature"):
            build_provider_planning_view(
                offer(signature="forged"), ack_status=True, **common)
        with self.assertRaisesRegex(ValueError, "expired"):
            build_provider_planning_view(
                offer(expires_at_ms=1500), ack_status=True, **common)
        with self.assertRaisesRegex(ValueError, "boot"):
            offer(boot_epoch="bad")

    def test_strategy_boundary_rejects_authority_and_sensitive_values(self):
        values = (
            {"provider_token": "secret"},
            {"raw_input": b"private"},
            {"working_directory": Path("/tmp")},
            {"callback": lambda: None},
        )
        for value in values:
            with self.subTest(value=repr(value)):
                with self.assertRaisesRegex(ValueError, "strategy boundary"):
                    reject_placement_sensitive(value)

        handle = socket.socket()
        try:
            with self.assertRaisesRegex(ValueError, "strategy boundary"):
                reject_placement_sensitive({"network_handle": handle})
        finally:
            handle.close()

    def test_assignment_is_candidate_bounded_and_aggregate_gpu_safe(self):
        provider = ProviderPlanningView(
            provider="/provider/a",
            service="/inference",
            boot_epoch="boot-epoch-0001",
            resource_sequence=1,
            offer_digest="sha256:" + "2" * 64,
            evidence_digest="sha256:" + "3" * 64,
            expires_at_ms=2_000_000_000_000,
            accepted_deadline_ms=2_000_000_000_000,
            accepted_roles=("stage-0", "stage-1"),
            backends=("onnxruntime",),
            usable_gpu_memory_mb=4096,
            queue_depth=0,
            estimated_wait_ms=0.0,
            rtt_ms=1.0,
            bandwidth_mbps=1000.0,
        )
        request = PlacementRequest(
            request_id="request-b",
            attempt=1,
            deadline_ms=2_000_000_000_000,
            model_digest="sha256:" + "d" * 64,
            graph_digest="sha256:" + "e" * 64,
            candidate_ids=("split-b",),
            providers=(provider,),
            required_roles=("stage-0", "stage-1"),
            budget=CandidateBudget(max_candidates=2, max_policy_ms=100),
        )

        class UnsafeStrategy(_DeterministicStrategy):
            def plan(self, value):
                return PlacementDecision(
                    split_id="split-b",
                    split_digest="sha256:" + "b" * 64,
                    assignments=(
                        ProviderAssignment(
                            "stage-0", "/provider/a", 3072, "onnxruntime"),
                        ProviderAssignment(
                            "stage-1", "/provider/a", 3072, "onnxruntime"),
                    ),
                    fallback_order={},
                    input_digest=value.digest(),
                    evidence_digest="sha256:" + "c" * 64,
                )

        with self.assertRaisesRegex(ValueError, "aggregate GPU"):
            evaluate_placement_strategy(UnsafeStrategy(), request)

    def test_time_budget_and_deterministic_replay_are_enforced(self):
        request = PlacementRequest(
            request_id="request-c",
            attempt=1,
            deadline_ms=2_000_000_000_000,
            model_digest="sha256:" + "d" * 64,
            graph_digest="sha256:" + "e" * 64,
            candidate_ids=("split-a",),
            providers=(),
            required_roles=("stage-0",),
            budget=CandidateBudget(max_candidates=1, max_policy_ms=1),
        )

        class SlowStrategy(_DeterministicStrategy):
            def plan(self, value):
                time.sleep(0.01)
                return super().plan(value)

        with self.assertRaisesRegex(TimeoutError, "time budget"):
            evaluate_placement_strategy(SlowStrategy(), request)

        class ChangingStrategy(_DeterministicStrategy):
            calls = 0

            def plan(self, value):
                self.calls += 1
                result = super().plan(value)
                if self.calls == 1:
                    return result
                return PlacementDecision(
                    **{
                        **result.__dict__,
                        "evidence_digest": "sha256:" + "9" * 64,
                    })

        with self.assertRaisesRegex(ValueError, "changed"):
            evaluate_placement_strategy(
                ChangingStrategy(), request, replay_deterministic=True)

    def test_contract_is_exported_from_sdk_and_app_contracts(self):
        from ndnsf_distributed_inference import sdk
        from ndnsf_distributed_inference.app_sdk import contracts

        self.assertIs(sdk.ModelPlacementStrategy, ModelPlacementStrategy)
        self.assertIs(
            sdk.ArtifactPreparationMode, ArtifactPreparationMode)
        self.assertIs(contracts.ProviderPlanningView, ProviderPlanningView)


if __name__ == "__main__":
    unittest.main()
