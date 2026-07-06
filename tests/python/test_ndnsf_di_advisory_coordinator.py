#!/usr/bin/env python3
"""Advisory coordinator contract tests for NDNSF-DI user-side planning."""

from __future__ import annotations

import unittest
from dataclasses import replace

from ndnsf_distributed_inference.runtime_v1 import (
    AdvisoryCoordinator,
    AdvisoryCoordinatorConfig,
    AdvisorySuggestion,
    DiProviderRuntimeState,
    FragmentResidency,
    GenericAckMetadata,
    GenericAdmissionLease,
    GenericProviderRuntimeHint,
    ModelFragmentKey,
    PlanIntent,
    PlanRole,
    PlanTemplate,
    choose_runtime_assignment,
    merge_advisory_suggestion,
)


FUTURE_MS = 4102444800000


def fragment() -> ModelFragmentKey:
    return ModelFragmentKey(
        model_id="qwen-tiny",
        model_digest="sha256:model",
        runtime_backend="onnx-cuda",
        precision="fp16",
        split_strategy="pipeline",
        stage_index=0,
        stage_count=1,
        layer_start=0,
        layer_end=13,
        fragment_digest="sha256:stage0",
    )


def template() -> PlanTemplate:
    return PlanTemplate(
        template_id="qwen-stage0-template",
        model_id="qwen-tiny",
        roles=(PlanRole("/Stage/0", fragment(), estimated_compute_ms=10),),
    )


def candidate(provider: str,
              residency: FragmentResidency = FragmentResidency.GPU_LOADED,
              *,
              lease_id: str = "",
              lease_expires_at_ms: int = FUTURE_MS,
              queue: int = 0) -> dict:
    lease = ()
    if lease_id:
        lease = (GenericAdmissionLease(
            lease_id=lease_id,
            request_id="req-any",
            service_name="/Inference/NativeTracer",
            provider_name=provider,
            expires_at_ms=lease_expires_at_ms,
            resource_binding={"roleId": "/Stage/0"},
        ),)
    state = DiProviderRuntimeState(
        provider_name=provider,
        fragment_states=(),
    )
    metadata = GenericAckMetadata(
        provider_runtime_hint=GenericProviderRuntimeHint(
            provider_name=provider,
            queue_length=queue,
            estimated_queue_wait_ms=queue * 10,
        ),
        lease_offers=lease,
        service_payload_schema="ndnsf-di-runtime-ack-v1",
        service_payload={
            **state.__dict__,
            "fragmentStates": [
                {
                    "fragmentKey": fragment(),
                    "residency": residency.value,
                    "estimatedReadyMs": 0,
                }
            ],
        },
    )
    return {"providerName": provider, "genericAckMetadata": metadata}


def intent(intent_id: str, request_id: str, *,
           created_at_ms: int = 1000) -> PlanIntent:
    return PlanIntent(
        intent_id=intent_id,
        request_id=request_id,
        user_name=f"/user/{intent_id}",
        template_id=template().template_id,
        created_at_ms=created_at_ms,
        expires_at_ms=created_at_ms + 5000,
        nonce=f"nonce-{intent_id}",
    )


class AdvisoryCoordinatorTest(unittest.TestCase):
    def test_disabled_coordinator_returns_no_suggestions(self) -> None:
        coordinator = AdvisoryCoordinator()
        self.assertEqual(
            coordinator.suggest(template(), {}, [intent("i1", "req-1")], now_ms_value=1000),
            {},
        )

    def test_invalid_or_expired_intents_do_not_produce_suggestions(self) -> None:
        tpl = template()
        coordinator = AdvisoryCoordinator(AdvisoryCoordinatorConfig(enabled=True))
        suggestions = coordinator.suggest(
            tpl,
            {
                "missing-request": {"/Stage/0": [candidate("/provider/a")]},
                "expired": {"/Stage/0": [candidate("/provider/b")]},
            },
            [
                PlanIntent(
                    intent_id="missing-request",
                    request_id="",
                    user_name="/user/a",
                    template_id=tpl.template_id,
                ),
                PlanIntent(
                    intent_id="expired",
                    request_id="req-expired",
                    user_name="/user/b",
                    template_id=tpl.template_id,
                    expires_at_ms=999,
                ),
            ],
            now_ms_value=1000,
        )

        self.assertEqual(suggestions, {})

    def test_coordinator_balances_two_users_across_equivalent_providers(self) -> None:
        tpl = template()
        intents = [intent("i1", "req-1"), intent("i2", "req-2", created_at_ms=1001)]
        candidates = {
            item.intent_id: {
                "/Stage/0": [
                    candidate("/provider/a", lease_id=f"lease-a-{item.intent_id}"),
                    candidate("/provider/b", lease_id=f"lease-b-{item.intent_id}"),
                ]
            }
            for item in intents
        }
        coordinator = AdvisoryCoordinator(AdvisoryCoordinatorConfig(
            enabled=True,
            fairness_penalty_ms=100,
            proof_secret="test-secret",
        ))

        suggestions = coordinator.suggest(tpl, candidates, intents, now_ms_value=1100)

        self.assertEqual(set(suggestions), {"i1", "i2"})
        first = suggestions["i1"].provider_for_role("/Stage/0")
        second = suggestions["i2"].provider_for_role("/Stage/0")
        self.assertNotEqual(first, second)
        self.assertEqual({first, second}, {"/provider/a", "/provider/b"})

    def test_merge_accepts_valid_fresh_suggestion(self) -> None:
        tpl = template()
        providers = {
            "/Stage/0": [
                candidate("/provider/a", FragmentResidency.GPU_LOADED, lease_id="lease-a"),
                candidate("/provider/b", FragmentResidency.CPU_RESIDENT, lease_id="lease-b"),
            ]
        }
        local = choose_runtime_assignment(tpl, providers, request_id="req-1")
        suggestions = AdvisoryCoordinator(AdvisoryCoordinatorConfig(
            enabled=True,
            proof_secret="test-secret",
        )).suggest(
            tpl,
            {"i1": providers},
            [intent("i1", "req-1")],
            now_ms_value=1100,
        )

        merged = merge_advisory_suggestion(
            local,
            suggestions["i1"],
            tpl,
            providers,
            proof_secret="test-secret",
            now_ms_value=1100,
        )

        self.assertEqual(merged.score_breakdown["advisoryStatus"], "accepted")
        self.assertEqual(
            merged.role_assignments["/Stage/0"]["provider"],
            suggestions["i1"].provider_for_role("/Stage/0"),
        )

    def test_stale_suggestion_is_ignored(self) -> None:
        tpl = template()
        providers = {"/Stage/0": [candidate("/provider/a"), candidate("/provider/b")]}
        local = choose_runtime_assignment(tpl, providers, request_id="req-1")
        suggestion = AdvisorySuggestion(
            suggestion_id="s1",
            intent_id="i1",
            request_id="req-1",
            template_id=tpl.template_id,
            role_assignments={"/Stage/0": {"provider": "/provider/b"}},
            coordinator_name="/coord",
            window_id="w1",
            created_at_ms=1000,
            expires_at_ms=1001,
        )

        merged = merge_advisory_suggestion(
            local,
            suggestion,
            tpl,
            providers,
            now_ms_value=2000,
        )

        self.assertEqual(merged.score_breakdown["advisoryStatus"], "ignored")
        self.assertEqual(merged.score_breakdown["advisoryReason"], "STALE_SUGGESTION")
        self.assertEqual(merged.role_assignments, local.role_assignments)

    def test_tampered_proof_is_ignored(self) -> None:
        tpl = template()
        providers = {"/Stage/0": [candidate("/provider/a"), candidate("/provider/b")]}
        local = choose_runtime_assignment(tpl, providers, request_id="req-1")
        suggestion = AdvisoryCoordinator(AdvisoryCoordinatorConfig(
            enabled=True,
            proof_secret="test-secret",
        )).suggest(tpl, {"i1": providers}, [intent("i1", "req-1")], now_ms_value=1100)["i1"]

        merged = merge_advisory_suggestion(
            local,
            replace(suggestion, proof="tampered"),
            tpl,
            providers,
            proof_secret="test-secret",
            now_ms_value=1100,
        )

        self.assertEqual(merged.score_breakdown["advisoryStatus"], "ignored")
        self.assertEqual(merged.score_breakdown["advisoryReason"], "ADVISORY_PROOF_INVALID")

    def test_suggestion_cannot_bypass_current_provider_lease_validation(self) -> None:
        tpl = template()
        local_providers = {
            "/Stage/0": [
                candidate("/provider/a", lease_id="lease-a"),
                candidate("/provider/b", lease_id="expired-b", lease_expires_at_ms=1000),
            ]
        }
        local = choose_runtime_assignment(tpl, local_providers, request_id="req-1")
        suggestion = AdvisorySuggestion(
            suggestion_id="s-valid-provider-b",
            intent_id="i1",
            request_id="req-1",
            template_id=tpl.template_id,
            role_assignments={"/Stage/0": {"provider": "/provider/b"}},
            coordinator_name="/coord",
            window_id="w1",
            created_at_ms=1100,
            expires_at_ms=2100,
        )

        merged = merge_advisory_suggestion(
            local,
            suggestion,
            tpl,
            local_providers,
            now_ms_value=1100,
        )

        self.assertEqual(merged.score_breakdown["advisoryStatus"], "ignored")
        self.assertEqual(
            merged.score_breakdown["advisoryReason"],
            "SUGGESTED_PROVIDER_NOT_CURRENTLY_VALID",
        )
        self.assertEqual(merged.role_assignments["/Stage/0"]["provider"], "/provider/a")


if __name__ == "__main__":
    unittest.main()
