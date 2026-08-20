"""Canonical model/task-first automatic collaboration planning surface."""

from __future__ import annotations

import base64
from dataclasses import dataclass, replace
import hashlib
import json
import re
import time
import uuid
from types import MappingProxyType
from typing import Any, Callable, Mapping, Protocol, Tuple

from ndnsf import CollaborationDependency, CollaborationRole

from ..adapters import ApplicationInput, ModelFamilyAdapter
from ..core.ports import CandidateBudget
from ..core.contracts import (
    DIRequestEnvelopeV2, DIRoleAssignmentV2, DISelectionAssignmentV2,
)
from ..plan import SealedCollaborationPlan
from ..sdk.placement import (
    ArtifactPreparationMode,
    DIProviderOfferV2,
    ModelPlacementStrategy,
    PlacementDecision,
    PlacementRequest,
    ProviderPlanningView,
    canonical_digest,
    build_provider_planning_view,
    evaluate_placement_strategy,
)
from ..splitter import SplitCandidate, canonical_contract_digest


def _require_digest(value: str, name: str) -> None:
    if not isinstance(value, str) or len(value) != 71 or not value.startswith(
            "sha256:"):
        raise ValueError(f"{name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(f"{name} must be a canonical sha256 digest") from exc


@dataclass(frozen=True)
class ModelRef:
    model_name: str
    content_digest: str
    semantics_digest: str
    source_revision: str | None = None

    def __post_init__(self) -> None:
        if not self.model_name:
            raise ValueError("model_name is required")
        _require_digest(self.content_digest, "content_digest")
        _require_digest(self.semantics_digest, "semantics_digest")

    @property
    def intent_digest(self) -> str:
        return canonical_digest(self)


@dataclass(frozen=True)
class InferenceTaskRef:
    task_name: str
    adapter_name: str
    adapter_descriptor_digest: str
    adapter_composition_digest: str
    task_descriptor_digest: str

    def __post_init__(self) -> None:
        if not self.task_name or not self.adapter_name:
            raise ValueError("inference task reference is incomplete")
        for name in (
                "adapter_descriptor_digest", "adapter_composition_digest",
                "task_descriptor_digest"):
            _require_digest(getattr(self, name), name)

    @classmethod
    def from_adapter(cls, adapter: ModelFamilyAdapter) -> "InferenceTaskRef":
        return cls(
            task_name=adapter.task.descriptor.task_name,
            adapter_name=adapter.descriptor.name,
            adapter_descriptor_digest=adapter.descriptor.descriptor_digest,
            adapter_composition_digest=adapter.composition_digest,
            task_descriptor_digest=canonical_contract_digest(
                adapter.task.descriptor),
        )


@dataclass(frozen=True)
class TaskOptions:
    schema_digest: str
    payload: bytes

    def __post_init__(self) -> None:
        _require_digest(self.schema_digest, "task options schema_digest")
        object.__setattr__(self, "payload", bytes(self.payload))


@dataclass(frozen=True)
class AutomaticInferenceHandle:
    collaboration: Any
    decision: PlacementDecision
    sealed_plan: SealedCollaborationPlan
    adapter: ModelFamilyAdapter
    planning_timings_ms: Mapping[str, float] = None

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "planning_timings_ms",
            MappingProxyType(dict(self.planning_timings_ms or {})),
        )

    def response(self, timeout_ms: int | None = None):
        return self.collaboration.result(timeout_ms)

    def result(self, timeout_ms: int | None = None) -> Any:
        response = self.response(timeout_ms)
        if not response.status:
            raise RuntimeError(response.error or "distributed inference failed")
        return self.adapter.task.decode_result(response.payload)


ProviderViewFactory = Callable[
    [Any, str, int], ProviderPlanningView
]
CatalogSnapshotProvider = Callable[[], Tuple[Any, ...]]


@dataclass(frozen=True)
class MaterializedSplit:
    """Trusted local materialization result for one generated split."""

    candidate_digest: str
    artifact_digests_by_role: Mapping[str, str]
    local_references_by_role: Mapping[str, str]

    def __post_init__(self) -> None:
        _require_digest(self.candidate_digest, "materialized candidate_digest")
        digests = dict(self.artifact_digests_by_role)
        references = dict(self.local_references_by_role)
        if not digests or set(digests) != set(references):
            raise ValueError("materialized split role coverage mismatch")
        for role, digest in digests.items():
            if not role or not references[role]:
                raise ValueError("materialized split contains an empty role/reference")
            _require_digest(digest, f"materialized artifact digest for {role}")
        object.__setattr__(
            self, "artifact_digests_by_role", MappingProxyType(digests))
        object.__setattr__(
            self, "local_references_by_role", MappingProxyType(references))


@dataclass(frozen=True)
class PublishedSplit:
    """Content-bound DistributedRepo publication visible to Providers."""

    candidate_digest: str
    artifact_digests_by_role: Mapping[str, str]
    artifact_data_names_by_role: Mapping[str, str]

    def __post_init__(self) -> None:
        _require_digest(self.candidate_digest, "published candidate_digest")
        digests = dict(self.artifact_digests_by_role)
        names = dict(self.artifact_data_names_by_role)
        if not digests or set(digests) != set(names):
            raise ValueError("published split role coverage mismatch")
        for role, digest in digests.items():
            if not role or not names[role].startswith("/"):
                raise ValueError("published artifact requires an absolute NDN name")
            _require_digest(digest, f"published artifact digest for {role}")
        object.__setattr__(
            self, "artifact_digests_by_role", MappingProxyType(digests))
        object.__setattr__(
            self, "artifact_data_names_by_role", MappingProxyType(names))


class SplitMaterializer(Protocol):
    """Trusted side-effect port that creates bytes for a generated split."""

    def materialize(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> MaterializedSplit:
        ...


class DistributedArtifactPublisher(Protocol):
    """Trusted port backed by NDNSF-DistributedRepo."""

    def publish(
        self,
        candidate: SplitCandidate,
        materialized: MaterializedSplit,
        *,
        deadline_ms: int,
    ) -> PublishedSplit:
        ...

    def resolve_existing(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> PublishedSplit:
        ...


class RejectGeneratedSplitMaterializer:
    """Fail closed when an experiment requires an existing pre-split."""

    def materialize(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> MaterializedSplit:
        del candidate, deadline_ms
        raise RuntimeError("generated split materialization is disabled")


class CatalogSnapshotArtifactPublisher:
    """Resolve exact active pre-split snapshots without republishing bytes."""

    def __init__(self, snapshot_provider: CatalogSnapshotProvider) -> None:
        self._snapshot_provider = snapshot_provider

    def publish(
        self,
        candidate: SplitCandidate,
        materialized: MaterializedSplit,
        *,
        deadline_ms: int,
    ) -> PublishedSplit:
        del candidate, materialized, deadline_ms
        raise RuntimeError("pre-split-only publisher cannot publish generated bytes")

    def resolve_existing(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> PublishedSplit:
        if int(time.time() * 1000) >= deadline_ms:
            raise TimeoutError("pre-split resolution deadline expired")
        matches = [
            value for value in self._snapshot_provider()
            if value.status == "ACTIVE"
            and value.candidate_digest == candidate.candidate_digest
            and value.model_content_digest == candidate.model.content_digest
            and value.semantics_digest == candidate.model.semantics_digest
            and value.graph_digest == candidate.graph_digest
        ]
        if len(matches) != 1:
            raise ValueError("exact active pre-split publication is not unique")
        snapshot = matches[0]
        names = {
            role: tuple(snapshot.artifact_data_names[role])
            for role in candidate.execution_plan.roles
        }
        if any(len(values) != 1 for values in names.values()):
            raise ValueError("automatic planning currently requires one artifact per role")
        return PublishedSplit(
            candidate_digest=candidate.candidate_digest,
            artifact_digests_by_role={
                role: candidate.artifacts_by_role[role][0]
                for role in candidate.execution_plan.roles
            },
            artifact_data_names_by_role={
                role: names[role][0]
                for role in candidate.execution_plan.roles
            },
        )


def v2_provider_view_factory(
    verify_offer_signature: Callable[[DIProviderOfferV2], bool],
) -> ProviderViewFactory:
    """Convert an authenticated generic ACK payload into a sanitized DI view."""
    if not callable(verify_offer_signature):
        raise TypeError("V2 Provider offer verifier must be callable")

    def convert(ack: Any, model_intent_digest: str,
                deadline_ms: int) -> ProviderPlanningView:
        offer = DIProviderOfferV2.from_bytes(bytes(ack.payload))
        return build_provider_planning_view(
            offer,
            ack_status=bool(ack.status),
            at_ms=int(time.time() * 1000),
            request_id=offer.request_id,
            attempt=offer.attempt,
            model_intent_digest=model_intent_digest,
            deadline_ms=deadline_ms,
            verify_signature=verify_offer_signature,
        )

    return convert

class AutomaticPlanningCoordinator:
    """Trusted composition around an untrusted, data-only placement decision."""

    def __init__(
        self,
        *,
        service_user: Any,
        service_name: str,
        adapters: Mapping[str, ModelFamilyAdapter],
        strategy: ModelPlacementStrategy,
        provider_view_factory: ProviderViewFactory,
        split_materializer: SplitMaterializer,
        artifact_publisher: DistributedArtifactPublisher,
        catalog_snapshot_provider: CatalogSnapshotProvider | None = None,
        budget: CandidateBudget | None = None,
        ack_timeout_ms: int = 300,
    ) -> None:
        if not service_name or not adapters:
            raise ValueError("automatic planning coordinator is incomplete")
        if not callable(getattr(split_materializer, "materialize", None)):
            raise TypeError("split_materializer does not implement materialize")
        if (not callable(getattr(artifact_publisher, "publish", None))
                or not callable(
                    getattr(artifact_publisher, "resolve_existing", None))):
            raise TypeError(
                "artifact_publisher does not implement publish/resolve_existing")
        if ack_timeout_ms <= 0:
            raise ValueError("ACK collection timeout must be positive")
        self.service_user = service_user
        self.service_name = str(service_name)
        self.adapters = MappingProxyType(dict(adapters))
        self.strategy = strategy
        self.provider_view_factory = provider_view_factory
        self.split_materializer = split_materializer
        self.artifact_publisher = artifact_publisher
        self.catalog_snapshot_provider = (
            catalog_snapshot_provider or (lambda: ()))
        self.budget = budget or CandidateBudget(
            max_candidates=16, max_policy_ms=100)
        self.ack_timeout_ms = int(ack_timeout_ms)

    def _resolve_adapter(
        self,
        task: InferenceTaskRef,
        application_input: ApplicationInput,
        options: TaskOptions | None,
    ) -> ModelFamilyAdapter:
        adapter = self.adapters.get(task.adapter_name)
        if adapter is None:
            raise ValueError("model adapter is not allowlisted")
        adapter.validate_pin(
            adapter_descriptor_digest=task.adapter_descriptor_digest,
            composition_digest=task.adapter_composition_digest,
        )
        if (task.task_name != adapter.task.descriptor.task_name
                or task.task_descriptor_digest != canonical_contract_digest(
                    adapter.task.descriptor)):
            raise ValueError("inference task descriptor pin mismatch")
        if (application_input.task_name != task.task_name
                or application_input.input_schema_digest !=
                adapter.descriptor.input_schema_digest
                or application_input.options_schema_digest !=
                adapter.descriptor.options_schema_digest):
            raise ValueError("ApplicationInput was not validated by this adapter")
        if options is not None and options.schema_digest != (
                adapter.descriptor.options_schema_digest):
            raise ValueError("task options schema mismatch")
        return adapter

    def request(
        self,
        *,
        model: ModelRef,
        task: InferenceTaskRef,
        input: ApplicationInput,
        timeout_ms: int,
        options: TaskOptions | None = None,
        objective: Any = None,
        constraints: Mapping[str, Any] | None = None,
        request_id: str = "",
    ) -> AutomaticInferenceHandle:
        request_started = time.perf_counter()
        timings: dict[str, float] = {}
        if timeout_ms <= self.ack_timeout_ms:
            raise ValueError("request timeout must exceed ACK collection")
        deadline_ms = int(time.time() * 1000) + timeout_ms
        request_id = request_id or (
            "/NDNSF/DI/REQUEST/" + uuid.uuid4().hex)
        invocation_id = "invocation:" + canonical_digest({
            "request_id": request_id, "model": model.intent_digest,
        })[7:39]
        phase_started = time.perf_counter()
        adapter = self._resolve_adapter(task, input, options)
        model_descriptor = adapter.describe_model(
            model.model_name,
            model.content_digest,
            model.semantics_digest,
            source_revision=model.source_revision or "",
        )
        graph = adapter.graph.inspect(model_descriptor)
        candidates = adapter.splitter.enumerate_candidates(
            model_descriptor, graph)
        timings["adapter_graph_split_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        if not candidates or len(candidates) > self.budget.max_candidates:
            raise ValueError("adapter returned an invalid candidate set")
        phase_started = time.perf_counter()
        request_payload = self._encode_request(
            model, task, input, options, deadline_ms, request_id,
            self.service_name, invocation_id)
        timings["request_encode_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        collaboration = self.service_user.begin_collaboration(
            self.service_name,
            request_payload,
            mode="DEFERRED",
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_id=request_id,
        )
        timings["request_publish_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        closed = collaboration.acks_closed()
        timings["ack_collect_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        providers = tuple(
            self.provider_view_factory(
                ack, model.intent_digest, deadline_ms)
            for ack in closed.candidates
            if ack.status
        )
        if not providers:
            raise ValueError("ACK_CLOSED contains no valid DI Provider offer")
        placement = PlacementRequest(
            request_id=collaboration.request_id,
            attempt=1,
            deadline_ms=deadline_ms,
            model_digest=model_descriptor.model_digest,
            graph_digest=graph.graph_digest,
            candidate_ids=tuple(
                candidate.candidate_digest for candidate in candidates),
            providers=providers,
            required_roles=candidates[0].execution_plan.roles,
            budget=self.budget,
            objective=objective,
            constraints=dict(constraints or {}),
            catalog_snapshot=tuple(self.catalog_snapshot_provider()),
            task_digest=task.task_descriptor_digest,
            state_contracts=adapter.state.contracts,
            model=model_descriptor,
            graph=graph,
            candidates=candidates,
        )
        phase_started = time.perf_counter()
        decision = evaluate_placement_strategy(self.strategy, placement)
        timings["placement_strategy_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        candidate = next(
            (item for item in candidates
             if item.candidate_digest == decision.split_id),
            None,
        )
        if candidate is None:
            raise ValueError("placement selected an unknown candidate")
        phase_started = time.perf_counter()
        published = self._prepare_artifacts(
            candidate, decision.artifact_preparation, deadline_ms)
        timings["artifact_resolve_publish_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        sealed = self._seal(
            closed.digest, placement, decision, candidate, published,
            invocation_id)
        timings["plan_seal_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=list(sealed.roles),
            key_scopes={
                key: list(value) for key, value in sealed.key_scopes.items()
            },
            dependencies=list(sealed.dependencies),
            artifact_data_names=dict(sealed.artifact_data_names),
            scope_key_data_names=dict(sealed.scope_key_data_names),
            role_scopes={
                key: list(value) for key, value in sealed.role_scopes.items()
            },
            role_provider_assignments=dict(sealed.providers_by_role),
            assignment_payloads_by_role=dict(
                sealed.assignment_payloads_by_role),
        )
        timings["selection_commit_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        timings["pre_response_setup_total_ms"] = (
            time.perf_counter() - request_started) * 1000.0
        return AutomaticInferenceHandle(
            collaboration, decision, sealed, adapter, timings)

    @staticmethod
    def _validate_published_split(
        candidate: SplitCandidate, published: PublishedSplit,
    ) -> None:
        expected = {
            role: candidate.artifacts_by_role[role][0]
            for role in candidate.execution_plan.roles
        }
        if (published.candidate_digest != candidate.candidate_digest
                or dict(published.artifact_digests_by_role) != expected
                or set(published.artifact_data_names_by_role) != set(expected)):
            raise ValueError(
                "published split does not match the selected candidate")

    def _prepare_artifacts(
        self,
        candidate: SplitCandidate,
        preparation: ArtifactPreparationMode,
        deadline_ms: int,
    ) -> PublishedSplit:
        if int(time.time() * 1000) >= deadline_ms:
            raise TimeoutError("artifact preparation deadline expired")
        if preparation is ArtifactPreparationMode.PRE_SPLIT:
            published = self.artifact_publisher.resolve_existing(
                candidate, deadline_ms=deadline_ms)
        else:
            materialized = self.split_materializer.materialize(
                candidate, deadline_ms=deadline_ms)
            expected = {
                role: candidate.artifacts_by_role[role][0]
                for role in candidate.execution_plan.roles
            }
            if (materialized.candidate_digest != candidate.candidate_digest
                    or dict(materialized.artifact_digests_by_role) != expected):
                raise ValueError(
                    "materialized split does not match the selected candidate")
            published = self.artifact_publisher.publish(
                candidate, materialized, deadline_ms=deadline_ms)
        self._validate_published_split(candidate, published)
        if int(time.time() * 1000) >= deadline_ms:
            raise TimeoutError("artifact publication completed after deadline")
        return published

    @staticmethod
    def _encode_request(
        model: ModelRef,
        task: InferenceTaskRef,
        application_input: ApplicationInput,
        options: TaskOptions | None,
        deadline_ms: int,
        request_id: str,
        service_name: str,
        invocation_id: str,
    ) -> bytes:
        options_payload = (
            options.payload if options is not None
            else application_input.options)
        input_manifest_digest = canonical_digest({
            "input_schema_digest": application_input.input_schema_digest,
            "options_schema_digest": application_input.options_schema_digest,
            "input_digest": hashlib.sha256(
                application_input.payload).hexdigest(),
            "options_digest": hashlib.sha256(options_payload).hexdigest(),
        })
        return DIRequestEnvelopeV2(
            invocation_id=invocation_id,
            request_id=request_id,
            attempt=1,
            service=service_name,
            model_name=model.model_name,
            model_identity_hash=model.intent_digest,
            task_kind=task.task_name,
            input_manifest_digest=input_manifest_digest,
            input_payload_b64=base64.b64encode(
                application_input.payload).decode("ascii"),
            options_payload_b64=base64.b64encode(
                options_payload).decode("ascii"),
            plan_deadline_ms=deadline_ms,
            security_domain="requester-default",
            model={
                "name": model.model_name,
                "identity_hash": model.intent_digest,
                "content_digest": model.content_digest,
                "semantics_digest": model.semantics_digest,
                "source_revision": model.source_revision,
            },
            task={
                "name": task.task_name,
                "adapter": task.adapter_name,
                "adapter_descriptor_digest":
                    task.adapter_descriptor_digest,
                "adapter_composition_digest":
                    task.adapter_composition_digest,
                "task_descriptor_digest": task.task_descriptor_digest,
            },
        ).to_bytes()

    def _seal(
        self,
        ack_closed_digest: str,
        placement: PlacementRequest,
        decision: PlacementDecision,
        candidate: SplitCandidate,
        published: PublishedSplit,
        invocation_id: str,
    ) -> SealedCollaborationPlan:
        assignments = {item.role: item for item in decision.assignments}
        roles = []
        artifact_names = {}
        assignment_payloads = {}
        provider_views = {item.provider: item for item in placement.providers}
        for role in candidate.execution_plan.roles:
            artifact_name = published.artifact_data_names_by_role[role]
            artifact_names[role] = artifact_name
            roles.append(CollaborationRole(
                role=role,
                service=self.service_name,
                artifact=artifact_name,
                allow_dynamic_provisioning=False,
            ))
        dependencies = []
        key_scopes: dict[str, tuple[str, ...]] = {}
        role_scopes: dict[str, list[str]] = {
            role: [] for role in candidate.execution_plan.roles
        }
        for index, dependency in enumerate(
                candidate.execution_plan.dependencies):
            scope = f"tensor-{index}-{canonical_digest(dependency)[7:23]}"
            dependencies.append(CollaborationDependency(
                producers=[dependency.producer],
                consumers=[dependency.consumer],
                key_scope=scope,
                topic_prefix="/activation",
                required=True,
            ))
            key_scopes[scope] = (
                dependency.producer, dependency.consumer)
            role_scopes[dependency.producer].append(scope)
            role_scopes[dependency.consumer].append(scope)
        plan_digest = canonical_digest({
            "ack_closed_digest": ack_closed_digest,
            "placement_input_digest": placement.digest(),
            "placement_decision_digest": decision.digest(),
            "candidate_digest": candidate.candidate_digest,
        })
        for provider in sorted(set(
                item.provider for item in assignments.values())):
            provider_roles = tuple(
                role for role in candidate.execution_plan.roles
                if assignments[role].provider == provider)
            view = provider_views[provider]
            layer_ranges = {
                role: self._role_layer_range(candidate, role)
                for role in provider_roles
            }
            role_tuple = tuple(
                DIRoleAssignmentV2(
                    role=role,
                    graph_node_id=",".join(sorted(
                        node for node, owner in
                        candidate.execution_plan.node_roles.items()
                        if owner == role)),
                    layer_start=layer_ranges[role][0],
                    layer_end=layer_ranges[role][1],
                    artifact_digest=candidate.artifacts_by_role[role][0],
                    dependency_digest=canonical_digest(tuple(
                        dependency for dependency in
                        candidate.execution_plan.dependencies
                        if dependency.producer == role
                        or dependency.consumer == role)),
                    adapter_id=candidate.model.adapter.name,
                    adapter_version=candidate.model.adapter.version,
                    required_gpu_mib=assignments[
                        role].required_gpu_memory_mb,
                    input_grant_digests=(canonical_digest({
                        "role": role,
                        "request_id": placement.request_id,
                        "attempt": placement.attempt,
                    }),),
                )
                for role in provider_roles
            )
            provider_assignment = DISelectionAssignmentV2(
                invocation_id=invocation_id,
                request_id=placement.request_id,
                attempt=placement.attempt,
                plan_digest=plan_digest,
                provider=provider,
                provider_boot_epoch=view.boot_epoch,
                offer_digest=view.offer_digest,
                resource_sequence=view.resource_sequence,
                roles=role_tuple,
                artifact_set_digest=canonical_digest({
                    role: candidate.artifacts_by_role[role]
                    for role in provider_roles
                }),
                dependency_graph_digest=canonical_contract_digest(
                    candidate.execution_plan),
                deadline_ms=placement.deadline_ms,
                generation=1,
            ).to_bytes()
            for role in provider_roles:
                assignment_payloads[role] = provider_assignment
        return SealedCollaborationPlan(
            ack_closed_digest=ack_closed_digest,
            placement_input_digest=placement.digest(),
            placement_decision_digest=decision.digest(),
            roles=tuple(roles),
            dependencies=tuple(dependencies),
            key_scopes=key_scopes,
            role_scopes={
                key: tuple(value) for key, value in role_scopes.items()
            },
            providers_by_role={
                role: assignment.provider
                for role, assignment in assignments.items()
            },
            artifact_data_names=artifact_names,
            scope_key_data_names={},
            assignment_payloads_by_role=assignment_payloads,
        )

    @staticmethod
    def _role_layer_range(
        candidate: SplitCandidate, role: str,
    ) -> tuple[int | None, int | None]:
        """Derive an exact half-open layer range from adapter graph node IDs."""
        layers = []
        for node, owner in candidate.execution_plan.node_roles.items():
            if owner != role:
                continue
            match = re.fullmatch(r"layer-(\d+)", node)
            if match is not None:
                layers.append(int(match.group(1)))
        if not layers:
            return None, None
        ordered = sorted(layers)
        if ordered != list(range(ordered[0], ordered[-1] + 1)):
            raise ValueError("role layer nodes do not form a contiguous range")
        return ordered[0], ordered[-1] + 1


def replan_placement_request(
    placement: PlacementRequest, *, at_ms: int,
    candidate_ids: tuple[str, ...],
    providers: tuple[ProviderPlanningView, ...],
) -> PlacementRequest:
    """Create a fresh-attempt immutable strategy input without extending time."""

    if at_ms >= placement.deadline_ms:
        raise TimeoutError("placement replan deadline expired")
    if not candidate_ids:
        raise ValueError("placement replan requires candidate coverage")
    return replace(
        placement,
        attempt=placement.attempt + 1,
        candidate_ids=tuple(candidate_ids),
        providers=tuple(providers),
    )


__all__ = [
    "AutomaticInferenceHandle",
    "AutomaticPlanningCoordinator",
    "InferenceTaskRef",
    "ModelRef",
    "TaskOptions",
    "replan_placement_request",
]
