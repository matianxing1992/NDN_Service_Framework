from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
import copy
import json
import unittest

import jsonschema
import yaml

from ndnsf.service import CollaborationRole, ServiceUser
from ndnsf_distributed_inference.adapters import (
    build_object_detection_adapter,
)
from ndnsf_distributed_inference.app_sdk.placement import (
    AckRoleCoveragePolicy,
    AutomaticPlanningCoordinator,
    GenerationRequest,
    InferenceTaskRef,
    MaterializedSplit,
    ModelRef,
    PublishedSplit,
)
from ndnsf_distributed_inference.app_sdk.client import (
    APPClient as CanonicalAPPClient,
)
from ndnsf_distributed_inference.app_sdk.contracts import (
    PreSplitCatalogSnapshot,
)
from ndnsf_distributed_inference.app_sdk.facades import (
    APPClient as NetworkAPPClient,
)
from ndnsf_distributed_inference.core.ports import CandidateBudget
from ndnsf_distributed_inference.core.contracts import (
    DATA_DRIVEN_V2,
    DISelectionAssignmentV2,
)
from ndnsf_distributed_inference.sdk.placement import (
    ArtifactPreparationMode,
    ModelPlacementStrategy,
    PlacementDecision,
    ProviderAssignment,
    ProviderPlanningView,
)
from ndnsf_distributed_inference.planner.presplit_first import (
    PreSplitFirstStrategy,
)
from ndnsf_distributed_inference.splitter import SplitSource


class _DeferredNative:
    def __init__(self):
        self.on_ack_closed = None
        self.ack_coverage_predicate = None
        self.commit_identity = None
        self.commit_count = 0
        self.begin_count = 0

    def begin_collaboration(
        self, service, payload, on_ack_closed, on_response, on_timeout,
        ack_timeout_ms, timeout_ms, request_id, **kwargs,
    ):
        self.begin_count += 1
        self.on_ack_closed = on_ack_closed
        self.on_response = on_response
        self.on_timeout = on_timeout
        self.ack_coverage_predicate = kwargs.get("ack_coverage_predicate")
        self.service = service
        self.payload = payload
        return request_id or "/request/deferred-1"

    def close_acks(self, *, digest="sha256:" + "a" * 64):
        candidate = SimpleNamespace(
            provider_name="/provider/a",
            service_name="/generic/work",
            request_id="/request/deferred-1",
            status=True,
            message="willing",
            payload=b"role=worker;",
            telemetry={"rtt_ms": 2.0},
        )
        self.on_ack_closed(SimpleNamespace(
            request_id="/request/deferred-1",
            candidates=[candidate],
            digest=digest,
            closed_at_us=1_000,
            request_deadline_us=20_000,
        ))

    def commit_collaboration_plan(self, *args):
        identity = repr(args)
        if self.commit_identity is None:
            self.commit_identity = identity
            self.commit_count += 1
            return True
        if identity == self.commit_identity:
            return True
        raise RuntimeError("conflicting second collaboration plan commit")


class _ClosedCollaboration:
    def __init__(self, events=None):
        self.request_id = "/request/automatic-1"
        self.commits = []
        self.events = events
        self.closed = SimpleNamespace(
            request_id=self.request_id,
            digest="sha256:" + "c" * 64,
            candidates=(
                SimpleNamespace(
                    provider_name="/provider/a",
                    request_id=self.request_id,
                    status=True,
                    payload=b"signed-offer",
                ),
            ),
        )

    def acks_closed(self):
        return self.closed

    def commit_plan(self, **kwargs):
        if self.events is not None:
            self.events.append("commit")
        self.commits.append(kwargs)
        return True


class _AutomaticServiceUser:
    def __init__(self, events=None):
        self.calls = []
        self.scope_key_calls = []
        self.collaboration = _ClosedCollaboration(events)

    def begin_collaboration(self, service, payload, **kwargs):
        request_id = str(kwargs.get("request_id") or self.collaboration.request_id)
        self.collaboration.request_id = request_id
        self.collaboration.closed.request_id = request_id
        for candidate in self.collaboration.closed.candidates:
            candidate.request_id = request_id
        self.calls.append((service, bytes(payload), dict(kwargs)))
        return self.collaboration

    def publish_encrypted_large_data(
        self, service, payload, *, object_label, freshness_ms,
    ):
        self.scope_key_calls.append({
            "service": service,
            "payload": bytes(payload),
            "object_label": object_label,
            "freshness_ms": int(freshness_ms),
        })
        return SimpleNamespace(
            success=True,
            encrypted_data_name=f"/encrypted/{object_label}",
            error="",
        )


class _OrderedClosedCollaboration(_ClosedCollaboration):
    def acks_closed(self):
        self.events.append("ack-closed")
        return super().acks_closed()


class _OrderedServiceUser(_AutomaticServiceUser):
    def __init__(self, events):
        self.calls = []
        self.scope_key_calls = []
        self.collaboration = _OrderedClosedCollaboration(events)
        self._events = events

    def begin_collaboration(self, service, payload, **kwargs):
        self._events.append("request")
        return super().begin_collaboration(service, payload, **kwargs)


class _OrderedAdapter:
    """Record graph/split work so the request/ACK ordering is executable."""

    def __init__(self, adapter, events):
        self._adapter = adapter
        self._events = events
        self.graph = SimpleNamespace(inspect=self._inspect)
        self.splitter = SimpleNamespace(
            enumerate_candidates=self._enumerate_candidates)

    def __getattr__(self, name):
        return getattr(self._adapter, name)

    def describe_model(self, *args, **kwargs):
        self._events.append("describe")
        return self._adapter.describe_model(*args, **kwargs)

    def _inspect(self, model):
        self._events.append("graph")
        return self._adapter.graph.inspect(model)

    def _enumerate_candidates(self, model, graph):
        self._events.append("split")
        return self._adapter.splitter.enumerate_candidates(model, graph)


class _GraphlessAdapter(_OrderedAdapter):
    def _inspect(self, model):
        del model
        self._events.append("graph")
        return None


class _GraphAwareStrategy(ModelPlacementStrategy):
    name = "graph-aware-test"
    version = "1"
    state_digest = "sha256:" + "d" * 64

    def plan(self, request):
        assert request.graph is not None
        assert request.candidates
        assert request.providers[0].rtt_ms == 2.0
        candidate = request.candidates[0]
        return PlacementDecision(
            split_id=candidate.candidate_digest,
            split_digest=candidate.candidate_digest,
            assignments=tuple(
                ProviderAssignment(role, "/provider/a", 1, "cpu")
                for role in candidate.execution_plan.roles
            ),
            fallback_order={},
            input_digest=request.digest(),
            evidence_digest="sha256:" + "e" * 64,
        )


class _NeverCalledStrategy(_GraphAwareStrategy):
    def __init__(self):
        self.called = False

    def plan(self, request):
        self.called = True
        return super().plan(request)


class _PlannerProbe:
    def __init__(self):
        self.calls = []
        self.generation_calls = []

    def request(self, **kwargs):
        self.calls.append(kwargs)
        return "automatic-handle"

    def generate(self, request):
        self.generation_calls.append(request)
        return "generation-handle"


class _Materializer:
    def __init__(self, events, *, fail=False):
        self.events = events
        self.fail = fail

    def materialize(self, candidate, *, deadline_ms):
        self.events.append("materialize")
        if self.fail:
            raise RuntimeError("materialization failed")
        digests = {
            role: candidate.artifacts_by_role[role][0]
            for role in candidate.execution_plan.roles
        }
        return MaterializedSplit(
            candidate_digest=candidate.candidate_digest,
            artifact_digests_by_role=digests,
            local_references_by_role={
                role: f"/tmp/materialized/{role}" for role in digests
            },
        )


class _Publisher:
    def __init__(
        self, events, *, fail=False, tamper=False, bad_name=False,
    ):
        self.events = events
        self.fail = fail
        self.tamper = tamper
        self.bad_name = bad_name

    @staticmethod
    def _published(candidate, *, tamper=False, bad_name=False):
        digests = {
            role: candidate.artifacts_by_role[role][0]
            for role in candidate.execution_plan.roles
        }
        if tamper:
            first = next(iter(digests))
            digests[first] = "sha256:" + "f" * 64
        return PublishedSplit(
            candidate_digest=candidate.candidate_digest,
            artifact_digests_by_role=digests,
            artifact_data_names_by_role={
                role: (
                    f"relative/model/{digest[7:]}" if bad_name
                    else f"/distributed-repo/model/{digest[7:]}"
                )
                for role, digest in digests.items()
            },
        )

    def publish(self, candidate, materialized, *, deadline_ms):
        self.events.append("publish")
        if self.fail:
            raise RuntimeError("publication failed")
        return self._published(
            candidate, tamper=self.tamper, bad_name=self.bad_name)

    def resolve_existing(self, candidate, *, deadline_ms):
        self.events.append("resolve-existing")
        if self.fail:
            raise RuntimeError("catalog resolution failed")
        return self._published(
            candidate, tamper=self.tamper, bad_name=self.bad_name)


class AutomaticCollaborationPlanTest(unittest.TestCase):
    def setUp(self):
        self.native = _DeferredNative()
        self.user = object.__new__(ServiceUser)
        self.user._native = self.native
        self.events = []

    def test_seal_accepts_transformers_cpu_assignment_from_signed_offer(self):
        """The final seal must preserve the strategy's CPU backend binding."""

        view = ProviderPlanningView(
            provider="/provider/cpu",
            service="/inference",
            boot_epoch="boot-epoch-cpu-1",
            resource_sequence=1,
            offer_digest="sha256:" + "1" * 64,
            evidence_digest="sha256:" + "2" * 64,
            expires_at_ms=10_000,
            accepted_deadline_ms=9_000,
            accepted_roles=("stage-0",),
            backends=("transformers-cpu",),
            devices=("cpu",),
            usable_gpu_memory_mb=1024,
            queue_depth=0,
            estimated_wait_ms=0.0,
            rtt_ms=1.0,
            bandwidth_mbps=1000.0,
        )
        assignment = ProviderAssignment(
            role="stage-0",
            provider=view.provider,
            required_gpu_memory_mb=512,
            backend="transformers-cpu",
            device="cpu",
        )

        self.assertEqual(
            AutomaticPlanningCoordinator._resolve_execution_device(
                assignment, view),
            "cpu",
        )

        with self.assertRaisesRegex(
                ValueError, "non-CPU backend cannot be assigned to CPU"):
            AutomaticPlanningCoordinator._resolve_execution_device(
                replace(assignment, backend="transformers"),
                replace(view, backends=("transformers",)),
            )

    def test_non_di_deferred_request_closes_once_then_commits_idempotently(self):
        invocation = self.user.begin_collaboration(
            "/generic/work",
            b"opaque application payload",
            mode="DEFERRED",
            ack_timeout_ms=100,
            timeout_ms=1000,
            request_id="/request/deferred-1",
        )
        with self.assertRaisesRegex(TimeoutError, "ACK_CLOSED"):
            invocation.acks_closed(timeout_ms=1)

        self.native.close_acks()
        closed = invocation.acks_closed()
        self.assertEqual(len(closed.candidates), 1)
        self.assertEqual(closed.candidates[0].payload, b"role=worker;")
        with self.assertRaises(TypeError):
            closed.candidates[0].telemetry["rtt_ms"] = 9.0

        commit = dict(
            ack_closed_digest=closed.digest,
            roles=[CollaborationRole("worker", service="/generic/work")],
            key_scopes={},
            role_provider_assignments={"worker": "/provider/a"},
            assignment_payloads_by_role={"worker": b"opaque-plan-bytes"},
        )
        self.assertTrue(invocation.commit_plan(**commit))
        self.assertTrue(invocation.commit_plan(**commit))
        self.assertEqual(self.native.begin_count, 1)
        self.assertEqual(self.native.commit_count, 1)

        with self.assertRaisesRegex(ValueError, "ACK_CLOSED"):
            invocation.commit_plan(**{
                **commit,
                "ack_closed_digest": "sha256:" + "b" * 64,
            })
        with self.assertRaisesRegex(RuntimeError, "conflicting"):
            invocation.commit_plan(**{
                **commit,
                "roles": [
                    CollaborationRole("other", service="/generic/work"),
                ],
            })
        with self.assertRaisesRegex(RuntimeError, "conflicting ACK_CLOSED"):
            self.native.close_acks(digest="sha256:" + "9" * 64)

    def test_deferred_request_accepts_application_ack_coverage_hook(self):
        observed = []
        invocation = self.user.begin_collaboration(
            "/generic/work",
            b"opaque application payload",
            mode="DEFERRED",
            ack_timeout_ms=100,
            timeout_ms=1000,
            request_id="/request/deferred-coverage-1",
            ack_coverage_predicate=lambda candidates: (
                observed.append(tuple(candidates)) or bool(candidates)),
        )
        self.assertEqual(invocation.request_id, "/request/deferred-coverage-1")
        candidate = SimpleNamespace(
            provider_name="/provider/a",
            service_name="/generic/work",
            request_id="/request/deferred-coverage-1",
            status=True,
            message="willing",
            payload=b"role=worker;",
            telemetry=None,
        )
        self.assertTrue(self.native.ack_coverage_predicate([candidate]))
        self.assertEqual(len(observed), 1)

    def test_role_coverage_is_only_a_validated_ack_hint(self):
        def provider_view(candidate, _model_digest, deadline_ms):
            self.assertGreater(deadline_ms, 0)
            return SimpleNamespace(
                accepted_roles=tuple(candidate.payload.decode().split(",")))

        policy = AckRoleCoveragePolicy(
            required_roles=("stage-0", "stage-1"),
            provider_view_factory=provider_view,
            model_intent_digest="sha256:" + "a" * 64,
            deadline_ms=1000,
        )
        make = lambda payload: SimpleNamespace(
            status=True, payload=payload.encode())
        self.assertFalse(policy((make("stage-0"),)))
        self.assertTrue(policy((make("stage-0,stage-1"),)))

    def test_generation_request_is_one_full_invocation_contract(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        application_input = adapter.task.encode_input(
            {"image_object": "/repo/input/1"}, {},
        )
        request = GenerationRequest(
            model=ModelRef(
                "example/detector",
                "sha256:" + "a" * 64,
                "sha256:" + "b" * 64,
            ),
            task=task,
            input=application_input,
            timeout_ms=1000,
        )
        self.assertEqual(request.output_mode, "FULL")
        self.assertEqual(request.timeout_ms, 1000)

    def test_preplanned_api_and_core_schema_remain_generic(self):
        root = Path(__file__).resolve().parents[2]
        header = (
            root / "ndn-service-framework" / "ServiceUser.hpp"
        ).read_text()
        source = (
            root / "ndn-service-framework" / "ServiceUser.cpp"
        ).read_text()
        self.assertIn("BeginCollaboration", header)
        self.assertIn("CommitCollaborationPlan", header)
        self.assertIn("RequestCollaboration", header)
        deferred_region = source[
            source.index("BeginCollaboration"):
            source.index("CommitCollaborationPlan") + 6000
        ]
        for forbidden in ("Qwen", "logits", "KV cache", "model fragment"):
            self.assertNotIn(forbidden, deferred_region)

    def test_model_task_first_request_plans_after_signed_ack_snapshot(self):
        service_user = _AutomaticServiceUser(self.events)
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        application_input = adapter.task.encode_input(
            {"image_object": "/repo/input/1"}, {},
        )

        def provider_view(ack, model_intent_digest, deadline_ms):
            self.assertEqual(ack.payload, b"signed-offer")
            self.assertTrue(model_intent_digest.startswith("sha256:"))
            return ProviderPlanningView(
                provider="/provider/a",
                service="/inference",
                boot_epoch="boot-epoch-0001",
                resource_sequence=1,
                offer_digest="sha256:" + "1" * 64,
                evidence_digest="sha256:" + "2" * 64,
                expires_at_ms=deadline_ms + 1000,
                accepted_deadline_ms=deadline_ms,
                accepted_roles=("stage-0", "stage-1"),
                backends=("cpu",),
                usable_gpu_memory_mb=1024,
                queue_depth=0,
                estimated_wait_ms=0.0,
                rtt_ms=2.0,
                bandwidth_mbps=100.0,
            )

        coordinator = AutomaticPlanningCoordinator(
            service_user=service_user,
            service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=_GraphAwareStrategy(),
            provider_view_factory=provider_view,
            split_materializer=_Materializer(self.events),
            artifact_publisher=_Publisher(self.events),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
        )
        model = ModelRef(
            "example/detector",
            "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
            source_revision="reviewed-revision",
        )
        handle = coordinator.generate(GenerationRequest(
            model=model,
            task=task,
            input=application_input,
            timeout_ms=1000,
            request_id="request/automatic-1",
        ))

        self.assertEqual(len(service_user.calls), 1)
        service, wire, kwargs = service_user.calls[0]
        self.assertEqual(service, "/inference")
        self.assertEqual(kwargs["mode"], "DEFERRED")
        request = json.loads(wire)
        self.assertEqual(request["request_id"], "/request%2Fautomatic-1")
        self.assertEqual(kwargs["request_id"], "/request%2Fautomatic-1")
        self.assertEqual(request["model"]["name"], "example/detector")
        self.assertEqual(request["task"]["generation_mode"], "FULL")
        self.assertEqual(
            request["model"]["content_digest"], model.content_digest)
        self.assertNotIn("deployment", request)
        self.assertEqual(len(service_user.collaboration.commits), 1)
        commit = service_user.collaboration.commits[0]
        self.assertEqual(
            set(commit["scope_key_data_names"]),
            set(commit["key_scopes"]),
        )
        self.assertEqual(
            set(handle.sealed_plan.scope_key_data_names),
            set(commit["key_scopes"]),
        )
        self.assertTrue(all(
            name.startswith("/encrypted/inference-scope-key-")
            for name in commit["scope_key_data_names"].values()
        ))
        self.assertEqual(
            set(commit["role_provider_assignments"]),
            {"stage-0", "stage-1"},
        )
        self.assertEqual(
            commit["ack_closed_digest"],
            service_user.collaboration.closed.digest,
        )
        assignment_payloads = commit["assignment_payloads_by_role"]
        self.assertEqual(
            assignment_payloads["stage-0"],
            assignment_payloads["stage-1"],
        )
        complete = DISelectionAssignmentV2.from_bytes(
            assignment_payloads["stage-0"])
        self.assertEqual(handle.sealed_plan.execution_policy, DATA_DRIVEN_V2)
        self.assertEqual(complete.execution_policy, DATA_DRIVEN_V2)
        self.assertEqual(complete.request_id, "/request%2Fautomatic-1")
        self.assertEqual(
            tuple(item.role for item in complete.roles),
            ("stage-0", "stage-1"),
        )
        self.assertEqual(complete.roles[0].required_input_scopes, ())
        self.assertEqual(len(complete.roles[1].required_input_scopes), 1)
        dynamic_scope = complete.roles[0].dependencies[0].key_scope
        self.assertEqual(
            complete.roles[1].dependencies[0].key_scope, dynamic_scope)
        self.assertEqual(
            complete.roles[1].required_input_scopes, (dynamic_scope,))
        self.assertIn(dynamic_scope, commit["key_scopes"])
        self.assertEqual(
            complete.roles[0].dependencies[0].tensors,
            ("tensor-0",),
        )
        self.assertEqual(
            handle.sealed_plan.ack_closed_digest,
            service_user.collaboration.closed.digest,
        )
        self.assertEqual(
            handle.sealed_plan.strategy_identity_digest,
            "sha256:" + __import__("hashlib").sha256(
                json.dumps({
                    "name": "graph-aware-test",
                    "state_digest": "sha256:" + "d" * 64,
                    "version": "1",
                }, sort_keys=True, separators=(",", ":")).encode("utf-8")
            ).hexdigest(),
        )
        self.assertEqual(self.events, ["materialize", "publish", "commit"])
        self.assertTrue(all(
            name.startswith("/distributed-repo/model/")
            for name in commit["artifact_data_names"].values()
        ))
        with self.assertRaises(TypeError):
            handle.sealed_plan.providers_by_role["stage-0"] = "/provider/b"
        with self.assertRaises(Exception) as frozen:
            handle.sealed_plan.ack_closed_digest = "sha256:" + "0" * 64
        self.assertEqual(type(frozen.exception).__name__, "FrozenInstanceError")

        with self.assertRaisesRegex(ValueError, "content_digest"):
            ModelRef("moving-name-only", "main", "sha256:" + "b" * 64)

    def test_artifact_side_effect_failures_are_zero_commit(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        application_input = adapter.task.encode_input(
            {"image_object": "/repo/input/1"}, {},
        )
        model = ModelRef(
            "example/detector",
            "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
            source_revision="reviewed-revision",
        )

        def provider_view(ack, model_intent_digest, deadline_ms):
            return ProviderPlanningView(
                provider="/provider/a",
                service="/inference",
                boot_epoch="boot-epoch-0001",
                resource_sequence=1,
                offer_digest="sha256:" + "1" * 64,
                evidence_digest="sha256:" + "2" * 64,
                expires_at_ms=deadline_ms + 1000,
                accepted_deadline_ms=deadline_ms,
                accepted_roles=("stage-0", "stage-1"),
                backends=("cpu",),
                usable_gpu_memory_mb=1024,
                queue_depth=0,
                estimated_wait_ms=0.0,
                rtt_ms=2.0,
                bandwidth_mbps=100.0,
            )

        for materialize_fail, publish_fail, tamper, bad_name, message in (
            (True, False, False, False, "materialization failed"),
            (False, True, False, False, "publication failed"),
            (False, False, True, False, "published split"),
            (False, False, False, True, "absolute NDN name"),
        ):
            with self.subTest(message=message):
                events = []
                service_user = _AutomaticServiceUser(events)
                coordinator = AutomaticPlanningCoordinator(
                    service_user=service_user,
                    service_name="/inference",
                    adapters={adapter.descriptor.name: adapter},
                    strategy=_GraphAwareStrategy(),
                    provider_view_factory=provider_view,
                    split_materializer=_Materializer(
                        events, fail=materialize_fail),
                    artifact_publisher=_Publisher(
                        events, fail=publish_fail, tamper=tamper,
                        bad_name=bad_name),
                    budget=CandidateBudget(
                        max_candidates=4, max_policy_ms=100),
                    ack_timeout_ms=100,
                )
                with self.assertRaisesRegex(
                        (RuntimeError, ValueError), message):
                    coordinator.request(
                        model=model,
                        task=task,
                        input=application_input,
                        timeout_ms=1000,
                    )
                self.assertEqual(service_user.collaboration.commits, [])

    def test_pre_split_candidate_resolves_catalog_without_materialization(self):
        adapter = build_object_detection_adapter()
        model = adapter.describe_model(
            "example/detector",
            "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
        )
        candidate = replace(
            adapter.splitter.enumerate_candidates(
                model, adapter.graph.inspect(model))[0],
            source=SplitSource.PRE_SPLIT,
        )
        events = []
        coordinator = AutomaticPlanningCoordinator(
            service_user=_AutomaticServiceUser(),
            service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=_GraphAwareStrategy(),
            provider_view_factory=lambda *args: None,
            split_materializer=_Materializer(events, fail=True),
            artifact_publisher=_Publisher(events),
            ack_timeout_ms=100,
        )
        published = coordinator._prepare_artifacts(
            candidate, ArtifactPreparationMode.PRE_SPLIT,
            deadline_ms=10**15)
        self.assertEqual(events, ["resolve-existing"])
        self.assertEqual(
            set(published.artifact_data_names_by_role),
            set(candidate.execution_plan.roles),
        )
        with self.assertRaisesRegex(TimeoutError, "deadline"):
            coordinator._prepare_artifacts(
                candidate,
                ArtifactPreparationMode.PRE_SPLIT,
                deadline_ms=1,
            )
        self.assertEqual(events, ["resolve-existing"])

    def test_automatic_request_injects_catalog_and_reuses_exact_publication(self):
        adapter = build_object_detection_adapter()
        task = InferenceTaskRef.from_adapter(adapter)
        application_input = adapter.task.encode_input(
            {"image_object": "/repo/input/1"}, {},
        )
        model = ModelRef(
            "example/detector",
            "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
            source_revision="reviewed-revision",
        )
        descriptor = adapter.describe_model(
            model.model_name,
            model.content_digest,
            model.semantics_digest,
            source_revision=model.source_revision,
        )
        graph = adapter.graph.inspect(descriptor)
        candidate = adapter.splitter.enumerate_candidates(descriptor, graph)[0]
        catalog = PreSplitCatalogSnapshot(
            alias="detector-exact",
            manifest_digest="sha256:" + "9" * 64,
            model_content_digest=descriptor.content_digest,
            semantics_digest=descriptor.semantics_digest,
            graph_digest=graph.graph_digest,
            candidate_digest=candidate.candidate_digest,
            backend="cpu",
            precision=descriptor.precision,
            artifact_data_names={
                role: (f"/repo/exact/{role}",)
                for role in candidate.execution_plan.roles
            },
            status="ACTIVE",
            created_at_ms=1,
        )

        def provider_view(ack, model_intent_digest, deadline_ms):
            return ProviderPlanningView(
                provider="/provider/a",
                service="/inference",
                boot_epoch="boot-epoch-0001",
                resource_sequence=1,
                offer_digest="sha256:" + "1" * 64,
                evidence_digest="sha256:" + "2" * 64,
                expires_at_ms=deadline_ms + 1000,
                accepted_deadline_ms=deadline_ms,
                accepted_roles=candidate.execution_plan.roles,
                backends=("cpu",),
                usable_gpu_memory_mb=1024,
                queue_depth=0,
                estimated_wait_ms=0.0,
                rtt_ms=2.0,
                bandwidth_mbps=100.0,
            )

        events = []
        service_user = _AutomaticServiceUser(events)
        coordinator = AutomaticPlanningCoordinator(
            service_user=service_user,
            service_name="/inference",
            adapters={adapter.descriptor.name: adapter},
            strategy=PreSplitFirstStrategy(at_ms=1),
            provider_view_factory=provider_view,
            split_materializer=_Materializer(events, fail=True),
            artifact_publisher=_Publisher(events),
            catalog_snapshot_provider=lambda: (catalog,),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
        )
        handle = coordinator.request(
            model=model,
            task=task,
            input=application_input,
            timeout_ms=1000,
        )
        self.assertIs(
            handle.decision.artifact_preparation,
            ArtifactPreparationMode.PRE_SPLIT,
        )
        self.assertEqual(events, ["resolve-existing", "commit"])
        self.assertEqual(len(service_user.collaboration.commits), 1)

    def test_request_is_published_before_graph_and_split_planning(self):
        base_adapter = build_object_detection_adapter()
        adapter = _OrderedAdapter(base_adapter, self.events)
        task = InferenceTaskRef.from_adapter(base_adapter)
        application_input = base_adapter.task.encode_input(
            {"image_object": "/repo/input/1"}, {},
        )
        model = ModelRef(
            "example/detector",
            "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
        )
        descriptor = base_adapter.describe_model(
            model.model_name, model.content_digest, model.semantics_digest)
        graph = base_adapter.graph.inspect(descriptor)
        candidate = base_adapter.splitter.enumerate_candidates(
            descriptor, graph)[0]

        def provider_view(_ack, _model_intent_digest, deadline_ms):
            return ProviderPlanningView(
                provider="/provider/a",
                service="/inference",
                boot_epoch="boot-epoch-0001",
                resource_sequence=1,
                offer_digest="sha256:" + "1" * 64,
                evidence_digest="sha256:" + "2" * 64,
                expires_at_ms=deadline_ms + 1000,
                accepted_deadline_ms=deadline_ms,
                accepted_roles=tuple(candidate.execution_plan.roles),
                backends=("cpu",),
                usable_gpu_memory_mb=1024,
                queue_depth=0,
                estimated_wait_ms=0.0,
                rtt_ms=2.0,
                bandwidth_mbps=100.0,
            )

        service_user = _OrderedServiceUser(self.events)
        coordinator = AutomaticPlanningCoordinator(
            service_user=service_user,
            service_name="/inference",
            adapters={base_adapter.descriptor.name: adapter},
            strategy=_GraphAwareStrategy(),
            provider_view_factory=provider_view,
            split_materializer=_Materializer(self.events),
            artifact_publisher=_Publisher(self.events),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
        )
        coordinator.request(
            model=model, task=task, input=application_input, timeout_ms=1000)

        self.assertLess(self.events.index("request"),
                        self.events.index("ack-closed"))
        self.assertLess(self.events.index("ack-closed"),
                        self.events.index("describe"))
        self.assertLess(self.events.index("graph"), self.events.index("split"))
        self.assertLess(self.events.index("split"),
                        self.events.index("materialize"))

    def test_external_strategy_is_not_called_without_dependency_graph(self):
        base_adapter = build_object_detection_adapter()
        adapter = _GraphlessAdapter(base_adapter, self.events)
        task = InferenceTaskRef.from_adapter(base_adapter)
        application_input = base_adapter.task.encode_input(
            {"image_object": "/repo/input/1"}, {},
        )

        def provider_view(_ack, _model_intent_digest, deadline_ms):
            return ProviderPlanningView(
                provider="/provider/a", service="/inference",
                boot_epoch="boot-epoch-0001", resource_sequence=1,
                offer_digest="sha256:" + "1" * 64,
                evidence_digest="sha256:" + "2" * 64,
                expires_at_ms=deadline_ms + 1000,
                accepted_deadline_ms=deadline_ms,
                accepted_roles=("stage-0", "stage-1"),
                backends=("cpu",), usable_gpu_memory_mb=1024,
                queue_depth=0, estimated_wait_ms=0.0,
                rtt_ms=2.0, bandwidth_mbps=100.0,
            )

        strategy = _NeverCalledStrategy()
        coordinator = AutomaticPlanningCoordinator(
            service_user=_OrderedServiceUser(self.events),
            service_name="/inference",
            adapters={base_adapter.descriptor.name: adapter},
            strategy=strategy,
            provider_view_factory=provider_view,
            split_materializer=_Materializer(self.events),
            artifact_publisher=_Publisher(self.events),
            budget=CandidateBudget(max_candidates=4, max_policy_ms=100),
            ack_timeout_ms=100,
        )
        model = ModelRef(
            "example/detector", "sha256:" + "a" * 64,
            "sha256:" + "b" * 64,
            source_revision="reviewed-revision",
        )
        with self.assertRaisesRegex(ValueError, "dependency graph snapshot"):
            coordinator.request(
                model=model, task=task, input=application_input,
                timeout_ms=1000)
        self.assertFalse(strategy.called)
        self.assertEqual(
            self.events, ["request", "ack-closed", "describe", "graph"])

    def test_public_clients_forward_only_model_task_input_request(self):
        planner = _PlannerProbe()
        owner = SimpleNamespace(_automatic_planner=planner)
        request = dict(
            model="model-ref",
            task="task-ref",
            input="validated-input",
            timeout_ms=1000,
            options="task-options",
            objective={"latency": "minimize"},
            constraints={"region": "lab"},
            request_id="/request/public-1",
        )
        self.assertEqual(
            CanonicalAPPClient.request(owner, **request),
            "automatic-handle",
        )
        self.assertEqual(
            NetworkAPPClient.request(owner, **request),
            "automatic-handle",
        )
        self.assertEqual(planner.calls, [request, request])
        for call in planner.calls:
            self.assertNotIn("deployment", call)
            self.assertNotIn("providers", call)
            self.assertNotIn("split", call)

    def test_public_clients_forward_one_generation_request(self):
        planner = _PlannerProbe()
        owner = SimpleNamespace(_automatic_planner=planner)
        generation = object()
        self.assertEqual(
            CanonicalAPPClient.generate(owner, generation),
            "generation-handle",
        )
        self.assertEqual(
            NetworkAPPClient.generate(owner, generation),
            "generation-handle",
        )
        self.assertEqual(planner.generation_calls, [generation, generation])

    def test_app_yaml_is_digest_pinned_and_rejects_deployment_or_secrets(self):
        root = Path(__file__).resolve().parents[2]
        contracts = (
            root / "specs" / "163-di-collaboration-planning" / "contracts"
        )
        schema = json.loads(
            (contracts / "app-config.schema.json").read_text())
        config = yaml.safe_load(
            (contracts / "app.example.yaml").read_text())

        # The sealed runtime image intentionally carries an older jsonschema.
        # This contract uses no 2020-12-only assertion keyword, so preserve
        # every constraint while translating only the definitions spelling for
        # the Draft 7 validator available in that image.
        if hasattr(jsonschema, "Draft202012Validator"):
            validator = jsonschema.Draft202012Validator(schema)
        else:
            compatible = copy.deepcopy(schema)
            compatible["$schema"] = "http://json-schema.org/draft-07/schema#"
            compatible["definitions"] = compatible.pop("$defs")

            def rewrite_refs(value):
                if isinstance(value, dict):
                    for key, item in value.items():
                        if key == "$ref" and isinstance(item, str):
                            value[key] = item.replace(
                                "#/$defs/", "#/definitions/")
                        else:
                            rewrite_refs(item)
                elif isinstance(value, list):
                    for item in value:
                        rewrite_refs(item)

            rewrite_refs(compatible)
            validator = jsonschema.Draft7Validator(compatible)
        validator.validate(config)

        self.assertRegex(
            config["planning"]["strategy"]["digest"],
            r"^sha256:[0-9a-f]{64}$",
        )
        self.assertNotIn("deployment", json.dumps(config).lower())
        self.assertNotIn("private_key", json.dumps(config).lower())
        self.assertNotIn("providers", json.dumps(config).lower())
        self.assertNotIn("roles", json.dumps(config).lower())

        forbidden = copy.deepcopy(config)
        forbidden["deployment"] = {"roles": ["stage-0"]}
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(forbidden)
        secret = copy.deepcopy(config)
        secret["identity"]["private_key"] = "must-not-be-configured"
        with self.assertRaises(jsonschema.ValidationError):
            validator.validate(secret)


if __name__ == "__main__":
    unittest.main()
