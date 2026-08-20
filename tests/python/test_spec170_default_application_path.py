from __future__ import annotations

import json
from dataclasses import replace
import hashlib
from types import SimpleNamespace
import unittest

from ndnsf_distributed_inference.adapters import build_object_detection_adapter
from ndnsf_distributed_inference.adapters.qwen import (
    QWEN36_STAGE_ROLES, build_qwen_three_stage_adapter,
)
from ndnsf_distributed_inference.app_sdk.placement import (
    AutomaticPlanningCoordinator, GenerationRequest, InferenceTaskRef,
    MaterializedSplit, ModelRef, PublishedSplit, v3_provider_view_factory,
)
from ndnsf_distributed_inference.app_sdk.canonical_artifacts import (
    CanonicalArtifactBinding,
)
from ndnsf_distributed_inference.core.ports import CandidateBudget
from ndnsf_distributed_inference.core import RedistributionEdge
from ndnsf_distributed_inference.planner.presplit_first import (
    PreSplitFirstStrategy,
)
from ndnsf_distributed_inference.sdk.placement import (
    DeviceTopologyProfile, ExecutionDisposition, ProviderOfferV3,
    PlacementProposalV3, ProviderSelectionProjectionV3,
    UNBOUND_GRAPH_DIGEST_V3,
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
    def __init__(self, *, accepted_roles=("stage-0", "stage-1"),
                 backends=("cpu",),
                 providers=("/provider/a", "/provider/b")):
        self.collaboration = None
        self.publish_calls = []
        self.begin_kwargs = None
        self.accepted_roles = tuple(accepted_roles)
        self.backends = tuple(backends)
        self.providers = tuple(providers)

    def begin_collaboration(self, service, payload, **kwargs):
        self.begin_kwargs = dict(kwargs)
        request_id = str(kwargs["request_id"])
        self.collaboration = _V3Collaboration(service, payload, request_id)
        # Provider offers are wildcarded before ACK_CLOSED; the coordinator
        # binds the real graph digest after the snapshot closes.
        for provider in self.providers:
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
                topology=DeviceTopologyProfile(provider, (), "cpu"),
                accepted_roles=self.accepted_roles,
                backends=self.backends,
                boot_epoch="boot-0001",
                captured_at_ms=1,
                expires_at_ms=10**15,
                signer_key_id="test-key",
                signature="test-signature",
            )
            public_key = (provider + ":selection-key").encode()
            self.collaboration.closed.candidates.append(SimpleNamespace(
                request_id=request_id, provider_name=provider, status=True,
                payload=offer.to_bytes(), selection_input_key_offer={
                    "schemaVersion": "1",
                    "recipient": provider,
                    "recipientCertName": provider + "/KEY/1/ID-CERT/0",
                    "recipientPublicKey": public_key.hex(),
                    "recipientCertDigest": (
                        "sha256:" + hashlib.sha256(public_key).hexdigest()),
                    "providerBootEpoch": "boot-0001",
                    "ndnsfDataV1EndpointPrefix": provider + "/data-v1",
                },
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


class _CertifiedCanonicalEnsurer(_CanonicalEnsurer):
    def describe(self, candidate):
        return CanonicalArtifactBinding(
            model_manifest_digest="sha256:" + "1" * 64,
            artifact_profile_digest="sha256:" + "2" * 64,
            graph_digest=candidate.graph_digest,
            canonical_initializer_digest="sha256:" + "3" * 64,
            adapter_descriptor_digest=candidate.model.adapter.descriptor_digest,
            assembler_descriptor_digest="sha256:" + "4" * 64,
            backend_abi="onnxruntime-test-cpu",
            canonical_source_bytes=1024 * 1024,
        )


class _CrossProviderStrategy(PreSplitFirstStrategy):
    """Force the two-stage test graph across two ACKed Providers."""

    def propose_v3(self, **kwargs):
        proposal = super().propose_v3(**kwargs)
        providers = sorted(item.provider for item in kwargs["providers"])
        roles = tuple(proposal.roles)
        if len(providers) < 2 or len(roles) < 2:
            raise ValueError("cross-Provider test requires two roles and Providers")
        return replace(proposal, provider_by_role={
            roles[0].role: providers[0],
            roles[1].role: providers[1],
        })


class _RecordingV3Strategy(PreSplitFirstStrategy):
    def __init__(self):
        super().__init__(at_ms=1)
        self.proposal_calls = 0

    def propose_v3(self, **kwargs):
        self.proposal_calls += 1
        return super().propose_v3(**kwargs)


class _FailingV3Strategy(PreSplitFirstStrategy):
    def __init__(self):
        super().__init__(at_ms=1)
        self.legacy_plan_called = False

    def propose_v3(self, **kwargs):
        raise ValueError("intentional V3 proposal failure")

    def plan(self, request):
        self.legacy_plan_called = True
        raise AssertionError("V3 failure must not invoke the V2 planner")


class DefaultApplicationPathTest(unittest.TestCase):
    @staticmethod
    def _object_detection_request(strategy):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        app_input = adapter.task.encode_input({"image_object": "/input/1"}, {})
        model = ModelRef(
            "example/detector", "sha256:" + "a" * 64,
            "sha256:" + "b" * 64, source_revision="immutable-revision")
        user = _V3ServiceUser()
        coordinator = AutomaticPlanningCoordinator(
            service_user=user, service_name="/inference",
            adapters={adapter.descriptor.name: adapter}, strategy=strategy,
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
            group_epoch_key_wrapper=lambda _public_key, _epoch_key: b"wrapped",
        )
        request = GenerationRequest(
            model=model, task=task, input=app_input, timeout_ms=5000,
            request_id="spec170-v3-strategy-contract")
        return coordinator, user, request

    def test_explicit_custom_v3_changes_only_proposal_generation(self):
        strategy = _RecordingV3Strategy()
        coordinator, user, request = self._object_detection_request(strategy)
        coordinator.generate(request)
        self.assertGreater(strategy.proposal_calls, 0)
        self.assertEqual(len(user.collaboration.commits), 1)
        wire = json.loads(user.collaboration._payload.decode())
        self.assertEqual(wire["task"]["placement_profile"], "DI_PLACEMENT_V3")

    def test_v3_failure_never_falls_back_to_v2(self):
        strategy = _FailingV3Strategy()
        coordinator, user, request = self._object_detection_request(strategy)
        with self.assertRaisesRegex(ValueError, "no feasible graph candidate"):
            coordinator.generate(request)
        self.assertFalse(strategy.legacy_plan_called)
        self.assertEqual(user.collaboration.commits, [])

    def test_qwen_hybrid_commit_preserves_every_rank_and_dependency(self):
        def digest(label):
            return "sha256:" + hashlib.sha256(label.encode()).hexdigest()

        degrees = (1, 2, 1)
        rank_artifacts = {
            role: tuple(digest(f"{role}:{rank}") for rank in range(degree))
            for role, degree in zip(QWEN36_STAGE_ROLES, degrees)
        }
        edges = (
            RedistributionEdge(
                (0,), (1, 2), "activation-0", "SCATTER", "epoch-1",
                digest("redistribution-0"), digest("layout-0"),
                digest("layout-1"), 4096),
            RedistributionEdge(
                (1, 2), (3,), "activation-1", "GATHER", "epoch-1",
                digest("redistribution-1"), digest("layout-1"),
                digest("layout-2"), 4096),
        )
        adapter = build_qwen_three_stage_adapter(
            model_name="Qwen/test", revision="immutable-test-revision",
            layer_ranges=((0, 1), (1, 2), (2, 3)),
            artifact_digests_by_role={
                role: values[0] for role, values in rank_artifacts.items()},
            weight_bytes_by_role={role: 1024 for role in QWEN36_STAGE_ROLES},
            tensor_degrees=degrees,
            rank_artifact_digests_by_role=rank_artifacts,
            redistributions=edges,
        )
        task = InferenceTaskRef.from_adapter(adapter)
        app_input = adapter.task.encode_input(b"prompt", {})
        model = ModelRef(
            "Qwen/test", digest("model"), digest("semantics"),
            source_revision="immutable-test-revision")
        user = _V3ServiceUser(
            accepted_roles=QWEN36_STAGE_ROLES,
            backends=("onnxruntime-cpu",),
            providers=(
                "/provider/a", "/provider/b", "/provider/c", "/provider/d"),
        )
        coordinator = AutomaticPlanningCoordinator(
            service_user=user, service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=PreSplitFirstStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
            group_epoch_key_wrapper=lambda _public_key, _epoch_key: b"wrapped",
        )
        coordinator.generate(GenerationRequest(
            model=model, task=task, input=app_input, timeout_ms=5000,
            request_id="spec170-v3-hybrid"))
        commit = user.collaboration.commits[0]
        expected_roles = {
            f"{QWEN36_STAGE_ROLES[0]}",
            f"{QWEN36_STAGE_ROLES[1]}#0",
            f"{QWEN36_STAGE_ROLES[1]}#1",
            f"{QWEN36_STAGE_ROLES[2]}",
        }
        self.assertEqual(set(commit["role_provider_assignments"]), expected_roles)
        self.assertEqual(set(commit["assignment_payloads_by_role"]), expected_roles)
        self.assertEqual(set(commit["artifact_data_names"]), expected_roles)
        self.assertEqual({role.role for role in commit["roles"]}, expected_roles)
        self.assertEqual(
            tuple(commit["dependencies"][0].producers),
            (QWEN36_STAGE_ROLES[0],))
        self.assertEqual(
            tuple(commit["dependencies"][0].consumers),
            (f"{QWEN36_STAGE_ROLES[1]}#0",
             f"{QWEN36_STAGE_ROLES[1]}#1"))
        projection = ProviderSelectionProjectionV3.from_bytes(
            next(iter(commit["assignment_payloads_by_role"].values())))
        self.assertEqual(
            projection.dependencies[0]["redistributions"][0]["operation"],
            "SCATTER")
        self.assertEqual(
            projection.dependencies[1]["redistributions"][0]["operation"],
            "GATHER")

    def test_default_presplit_strategy_uses_v3_and_provider_assembly(self):
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
            strategy=PreSplitFirstStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
            group_epoch_key_wrapper=lambda _public_key, _epoch_key: b"wrapped",
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
            strategy=PreSplitFirstStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            canonical_artifact_ensurer=ensurer,
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
            group_epoch_key_wrapper=lambda _public_key, _epoch_key: b"wrapped",
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

    def test_v3_seals_exact_provider_assembly_recipe_after_ack_placement(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        app_input = adapter.task.encode_input({"image_object": "/input/1"}, {})
        model = ModelRef(
            "example/detector", "sha256:" + "a" * 64,
            "sha256:" + "b" * 64, source_revision="immutable-revision",
        )
        user = _V3ServiceUser()
        ensurer = _CertifiedCanonicalEnsurer()
        coordinator = AutomaticPlanningCoordinator(
            service_user=user, service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=PreSplitFirstStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            canonical_artifact_ensurer=ensurer,
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
            group_epoch_key_wrapper=lambda _public_key, _epoch_key: b"wrapped",
        )
        coordinator.generate(GenerationRequest(
            model=model, task=task, input=app_input, timeout_ms=5000,
            request_id="spec170-v3-certified-assembly",
        ))
        self.assertEqual(len(ensurer.calls), 1)
        certified = ensurer.calls[0][1]
        self.assertTrue(all(item.model_manifest_digest for item in certified))
        self.assertTrue(all(item.canonical_initializer_digest for item in certified))
        self.assertTrue(all(item.node_indices for item in certified))
        self.assertTrue(all(item.expected_inputs for item in certified))
        self.assertTrue(all(item.expected_outputs for item in certified))
        self.assertTrue(all(item.precision for item in certified))
        self.assertTrue(all(item.resource_envelope["maxSourceBytes"]
                            == 1024 * 1024 for item in certified))
        for payload in user.collaboration.commits[0][
                "assignment_payloads_by_role"].values():
            projection = ProviderSelectionProjectionV3.from_bytes(payload)
            self.assertTrue(projection.assembly.node_indices)
            self.assertTrue(projection.assembly.expected_inputs)
            self.assertTrue(projection.assembly.expected_outputs)

    def test_v3_cross_provider_selection_carries_one_sealed_data_capability(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        app_input = adapter.task.encode_input({"image_object": "/input/1"}, {})
        model = ModelRef(
            "example/detector", "sha256:" + "a" * 64,
            "sha256:" + "b" * 64, source_revision="immutable-revision",
        )
        user = _V3ServiceUser()
        wrapped = []

        def wrap_key(public_key: bytes, epoch_key: bytes) -> bytes:
            wrapped.append((bytes(public_key), bytes(epoch_key)))
            return hashlib.sha256(public_key + epoch_key).digest()

        coordinator = AutomaticPlanningCoordinator(
            service_user=user, service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=_CrossProviderStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100, group_epoch_key_wrapper=wrap_key,
        )
        coordinator.generate(GenerationRequest(
            model=model, task=task, input=app_input, timeout_ms=5000,
            request_id="spec170-v3-cross-provider",
        ))

        self.assertEqual(
            user.begin_kwargs["request_capabilities"],
            {"NDNSF_DATA_V1": "required"},
        )
        commit = user.collaboration.commits[0]
        self.assertEqual(
            set(commit["role_provider_assignments"].values()),
            {"/provider/a", "/provider/b"},
        )
        capabilities = {
            ProviderSelectionProjectionV3.from_bytes(payload).group_capability_v1
            for payload in commit["assignment_payloads_by_role"].values()
        }
        self.assertEqual(len(capabilities), 2)
        self.assertTrue(all(capabilities))
        self.assertTrue(all(
            capability_hex == capability_hex.lower()
            for capability_hex in capabilities))
        self.assertEqual(len(wrapped), 2)
        self.assertEqual(wrapped[0][1], wrapped[1][1])
        self.assertEqual(len(wrapped[0][1]), 32)

    def test_v3_cross_provider_selection_rejects_missing_key_offer(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        app_input = adapter.task.encode_input({"image_object": "/input/1"}, {})
        model = ModelRef(
            "example/detector", "sha256:" + "a" * 64,
            "sha256:" + "b" * 64, source_revision="immutable-revision",
        )
        user = _V3ServiceUser()
        original_begin = user.begin_collaboration

        def begin_without_second_offer(*args, **kwargs):
            collaboration = original_begin(*args, **kwargs)
            collaboration.closed.candidates[1].selection_input_key_offer = {}
            return collaboration

        user.begin_collaboration = begin_without_second_offer
        coordinator = AutomaticPlanningCoordinator(
            service_user=user, service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=_CrossProviderStrategy(at_ms=1),
            provider_view_factory=v3_provider_view_factory(lambda offer: True),
            split_materializer=SimpleNamespace(materialize=lambda *a, **k: None),
            artifact_publisher=_PublisherThatMustNotRun(),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
            group_epoch_key_wrapper=lambda public_key, key: b"wrapped",
        )
        with self.assertRaisesRegex(ValueError, "key offer"):
            coordinator.generate(GenerationRequest(
                model=model, task=task, input=app_input, timeout_ms=5000,
                request_id="spec170-v3-missing-key-offer",
            ))
        self.assertEqual(user.collaboration.commits, [])


if __name__ == "__main__":
    unittest.main()
