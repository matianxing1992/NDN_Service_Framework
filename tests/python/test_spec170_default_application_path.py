from __future__ import annotations

import json
from types import SimpleNamespace
import unittest

from ndnsf_distributed_inference.adapters import build_object_detection_adapter
from ndnsf_distributed_inference.app_sdk.placement import (
    AutomaticPlanningCoordinator, GenerationRequest, InferenceTaskRef,
    MaterializedSplit, ModelRef, PublishedSplit, v3_provider_view_factory,
)
from ndnsf_distributed_inference.core.ports import CandidateBudget
from ndnsf_distributed_inference.planner.layer_reuse_first import (
    LayerReuseFirstStrategy,
)
from ndnsf_distributed_inference.sdk.placement import (
    DeviceTopologyProfile, ExecutionDisposition, ProviderOfferV3,
    ProviderSelectionProjectionV3, UNBOUND_GRAPH_DIGEST_V3,
)


class _V3Collaboration:
    def __init__(self, service: str, payload: bytes, request_id: str):
        self.request_id = request_id
        self.commits = []
        self.closed = SimpleNamespace(
            request_id=request_id,
            digest="sha256:" + "c" * 64,
            candidates=[],
        )
        self._payload = bytes(payload)
        self._service = service

    def acks_closed(self):
        return self.closed

    def commit_plan(self, **kwargs):
        self.commits.append(kwargs)
        return True


class _V3ServiceUser:
    def __init__(self):
        self.collaboration = None
        self.publish_calls = []

    def begin_collaboration(self, service, payload, **kwargs):
        request_id = str(kwargs["request_id"])
        self.collaboration = _V3Collaboration(service, payload, request_id)
        # Provider offers are wildcarded before ACK_CLOSED; the coordinator
        # binds the real graph digest after the snapshot closes.
        for provider in ("/provider/a", "/provider/b"):
            offer = ProviderOfferV3(
                request_id=request_id,
                attempt=1,
                service=service,
                provider=provider,
                model_digest=_model_intent_from_wire(payload),
                graph_digest=UNBOUND_GRAPH_DIGEST_V3,
                status=True,
                execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
                preparation_accepted=True,
                topology=DeviceTopologyProfile(provider, ("cpu",), "cpu"),
                accepted_roles=("stage-0", "stage-1"),
                backends=("cpu",),
                boot_epoch="boot-0001",
                captured_at_ms=1,
                expires_at_ms=10**15,
                signer_key_id="test-key",
                signature="test-signature",
            )
            self.collaboration.closed.candidates.append(SimpleNamespace(
                request_id=request_id, status=True, payload=offer.to_bytes(),
            ))
        return self.collaboration

    def publish_encrypted_large_data(self, service, payload, *, object_label,
                                     freshness_ms):
        self.publish_calls.append((service, object_label, bytes(payload)))
        return SimpleNamespace(
            success=True,
            encrypted_data_name=f"/encrypted/{object_label}",
            error="",
        )


def _model_intent_from_wire(payload: bytes) -> str:
    return json.loads(payload.decode())["model"]["identity_hash"]


class _PublisherThatMustNotRun:
    def publish(self, *args, **kwargs):  # pragma: no cover - assertion path
        raise AssertionError("V3 requester must not publish a role split")

    def resolve_existing(self, *args, **kwargs):  # pragma: no cover
        raise AssertionError("V3 requester must not resolve role artifacts")


class _CanonicalEnsurer:
    def __init__(self):
        self.calls = []

    def ensure(self, candidate, role_specs, *, deadline_ms):
        self.calls.append((candidate.candidate_digest, tuple(role_specs), deadline_ms))
        return PublishedSplit(
            candidate_digest=candidate.candidate_digest,
            artifact_digests_by_role={
                role: candidate.artifacts_by_role[role][0]
                for role in candidate.execution_plan.roles
            },
            artifact_data_names_by_role={
                role: f"/canonical/{role.replace('-', '/')}/object"
                for role in candidate.execution_plan.roles
            },
        )


class DefaultApplicationPathTest(unittest.TestCase):
    def test_default_layer_reuse_strategy_uses_v3_and_provider_assembly(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        app_input = adapter.task.encode_input({"image_object": "/input/1"}, {})
        model = ModelRef(
            "example/detector", "sha256:" + "a" * 64,
            "sha256:" + "b" * 64, source_revision="immutable-revision",
        )
        user = _V3ServiceUser()
        coordinator = AutomaticPlanningCoordinator(
            service_user=user,
            service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=LayerReuseFirstStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
        )
        handle = coordinator.generate(GenerationRequest(
            model=model, task=task, input=app_input, timeout_ms=5000,
            request_id="spec170-v3-default",
        ))
        self.assertEqual(len(user.collaboration.commits), 1)
        commit = user.collaboration.commits[0]
        wire = json.loads(user.collaboration._payload.decode())
        self.assertEqual(wire["task"]["placement_profile"], "DI_PLACEMENT_V3")
        self.assertEqual(set(commit["role_provider_assignments"]),
                         {"stage-0", "stage-1"})
        for payload in commit["assignment_payloads_by_role"].values():
            projection = ProviderSelectionProjectionV3.from_bytes(payload)
            self.assertEqual(projection.schema, "ndnsf-di-selection-v3")
            self.assertTrue(projection.plan_digest.startswith("sha256:"))
            self.assertEqual(projection.roles[0].role_kind, "PIPELINE_RANGE")
        self.assertEqual(handle.decision.evidence["placementProfile"],
                         "DI_PLACEMENT_V3")
        self.assertEqual(handle.decision.artifact_preparation.value,
                         "GENERATED")

    def test_v3_ensures_canonical_artifacts_only_after_ack_closed(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        app_input = adapter.task.encode_input({"image_object": "/input/1"}, {})
        model = ModelRef(
            "example/detector", "sha256:" + "a" * 64,
            "sha256:" + "b" * 64, source_revision="immutable-revision",
        )
        user = _V3ServiceUser()
        ensurer = _CanonicalEnsurer()
        coordinator = AutomaticPlanningCoordinator(
            service_user=user,
            service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=LayerReuseFirstStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            canonical_artifact_ensurer=ensurer,
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
        )
        coordinator.generate(GenerationRequest(
            model=model, task=task, input=app_input, timeout_ms=5000,
            request_id="spec170-v3-canonical-ensure",
        ))
        self.assertEqual(len(ensurer.calls), 1)
        commit = user.collaboration.commits[0]
        self.assertTrue(all(
            value.startswith("/canonical/")
            for value in commit["artifact_data_names"].values()))


if __name__ == "__main__":
    unittest.main()
