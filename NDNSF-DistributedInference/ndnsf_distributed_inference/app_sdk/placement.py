"""Canonical model/task-first automatic collaboration planning surface."""

from __future__ import annotations

import base64
from dataclasses import dataclass, field, replace
import hashlib
import json
import math
import re
import secrets
import time
import uuid
from types import MappingProxyType
from typing import Any, Callable, Mapping, Protocol, Tuple
from urllib.parse import quote

from ndnsf import CollaborationDependency, CollaborationRole

from ..adapters import ApplicationInput, ModelFamilyAdapter
from .contracts import GenerationConfig, GenerationInput
from ..core.ports import CandidateBudget
from ..core.contracts import (
    DATA_DRIVEN_V2, DIDataDependencyV2, DIRequestEnvelopeV2, DIRoleAssignmentV2,
    DISelectionAssignmentV2,
)
from ..plan import SealedCollaborationPlan
from ..sdk.placement import (
    ArtifactPreparationMode,
    DIProviderOfferV2,
    DI_PLACEMENT_V3,
    UNBOUND_GRAPH_DIGEST_V3,
    ExecutionDisposition,
    ModelPlacementStrategy,
    PlacementDecision,
    PlacementProposalV3,
    PlacementRequest,
    PlanSealerV3,
    ProviderAssignment,
    ProviderGrantViewV1,
    ProviderOfferV3,
    ProviderPlanningViewV3,
    ProviderSelectionProjectionV3,
    ProviderPlanningView,
    RoleAssemblySpec,
    canonical_digest,
    build_provider_planning_view,
    evaluate_placement_strategy,
    is_cpu_backend,
)
from ..splitter import SplitCandidate, SplitSource, canonical_contract_digest


def _require_digest(value: str, name: str) -> None:
    if not isinstance(value, str) or len(value) != 71 or not value.startswith(
            "sha256:"):
        raise ValueError(f"{name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(f"{name} must be a canonical sha256 digest") from exc


def normalize_request_id_component(request_id: str) -> str:
    """Return one canonical V2 request-ID NameComponent URI.

    NDNSF V2 appends exactly one request-ID component after a variable-length
    service name. Human-readable IDs may contain ``/`` separators, so encode
    those separators inside the component instead of silently extending the
    service name on the wire.
    """

    value = str(request_id or "").strip()
    if not value:
        raise ValueError("request_id must not be empty")
    body = value[1:] if value.startswith("/") else value
    if not body:
        raise ValueError("request_id must not be the root name")
    component = quote(body, safe="-._~%")
    if not component or "/" in component:
        raise ValueError("request_id must encode to one NameComponent")
    return "/" + component


@dataclass(frozen=True, init=False)
class ModelRef:
    model_name: str
    content_digest: str
    semantics_digest: str
    source_revision: str | None = None

    def __init__(
        self,
        model_name: str | None = None,
        content_digest: str = "",
        semantics_digest: str = "",
        source_revision: str | None = None,
        *,
        name: str | None = None,
        revision: str | None = None,
        tokenizer_digest: str | None = None,
    ) -> None:
        resolved_name = str(name or model_name or "")
        resolved_revision = revision if revision is not None else source_revision
        resolved_semantics = str(tokenizer_digest or semantics_digest or "")
        if name is not None and model_name is not None and name != model_name:
            raise ValueError("model name aliases disagree")
        if (revision is not None and source_revision is not None
                and revision != source_revision):
            raise ValueError("model revision aliases disagree")
        if (tokenizer_digest is not None and semantics_digest
                and tokenizer_digest != semantics_digest):
            raise ValueError("tokenizer/semantics digest aliases disagree")
        object.__setattr__(self, "model_name", resolved_name)
        object.__setattr__(self, "content_digest", str(content_digest))
        object.__setattr__(self, "semantics_digest", resolved_semantics)
        object.__setattr__(self, "source_revision", resolved_revision)
        self.__post_init__()

    def __post_init__(self) -> None:
        if not self.model_name:
            raise ValueError("model_name is required")
        _require_digest(self.content_digest, "content_digest")
        _require_digest(self.semantics_digest, "semantics_digest")

    @property
    def intent_digest(self) -> str:
        return canonical_digest(self)

    @property
    def name(self) -> str:
        return self.model_name

    @property
    def revision(self) -> str | None:
        return self.source_revision

    @property
    def tokenizer_digest(self) -> str:
        return self.semantics_digest


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
class GenerationRequest:
    """One complete model-generation invocation.

    ``input`` contains the complete prompt/prefill payload.  The request is
    submitted once; autoregressive decode is adapter/provider data-plane
    work, and the normative wire result is one complete response.  It must
    not call the planner again for individual output tokens.
    """

    model: ModelRef
    task: InferenceTaskRef
    input: ApplicationInput
    timeout_ms: int
    options: TaskOptions | None = None
    objective: Any = None
    constraints: Mapping[str, Any] = field(default_factory=dict)
    request_id: str = ""
    output_mode: str = "FULL"

    def __post_init__(self) -> None:
        if self.timeout_ms <= 0 or self.output_mode != "FULL":
            raise ValueError("generation requires a positive FULL-output timeout")
        object.__setattr__(self, "constraints", MappingProxyType(
            {str(key): value for key, value in self.constraints.items()}))


# Request-scoped control traffic for an application-owned autoregressive loop.
# It is deliberately not a model dependency edge: the static model graph stays
# acyclic, while the provider handlers may exchange bounded generation control
# records over this separately authorized scope.
GENERATION_CONTROL_SCOPE = "generation-control-v1"


@dataclass(frozen=True)
class AutomaticInferenceHandle:
    collaboration: Any
    decision: PlacementDecision
    sealed_plan: SealedCollaborationPlan
    adapter: ModelFamilyAdapter
    planning_timings_ms: Mapping[str, float] = None
    invocation_id: str = ""

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
class AckRoleCoveragePolicy:
    """Application-owned early ACK closure for known required role coverage.

    This policy is intentionally only a coverage predicate.  It validates each
    positive ACK through the configured DI provider-view factory, then reports
    whether the bounded role hint is covered.  Graph inspection, split
    enumeration, provider assignment, artifact publication, and Selection
    remain after the immutable ACK_CLOSED snapshot.
    """

    required_roles: tuple[str, ...]
    provider_view_factory: ProviderViewFactory
    model_intent_digest: str
    deadline_ms: int

    def __post_init__(self) -> None:
        roles = tuple(str(role) for role in self.required_roles)
        if (not roles or any(not role for role in roles)
                or len(set(roles)) != len(roles)):
            raise ValueError("ACK role coverage requires unique non-empty roles")
        if not callable(self.provider_view_factory):
            raise TypeError("provider_view_factory must be callable")
        object.__setattr__(self, "required_roles", roles)

    def __call__(self, candidates: tuple[Any, ...]) -> bool:
        covered: set[str] = set()
        for candidate in candidates:
            if not bool(getattr(candidate, "status", False)):
                continue
            try:
                view = self.provider_view_factory(
                    candidate, self.model_intent_digest, self.deadline_ms)
            except (TypeError, ValueError, RuntimeError):
                # Invalid or stale offers cannot contribute to early closure;
                # the normal ACK timeout still provides the final chance to
                # collect a valid offer.
                continue
            covered.update(str(role) for role in view.accepted_roles)
        return set(self.required_roles).issubset(covered)


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


class CanonicalArtifactEnsurer(Protocol):
    """Request-first V3 port for canonical layer publication/lookup.

    Unlike ``DistributedArtifactPublisher`` this port is not a role-split
    compatibility path.  It is invoked only after ACK_CLOSED and graph
    planning, and returns the content-addressed Data names that become part of
    the sealed Selection.  Implementations may resolve an exact existing
    publication or publish missing canonical objects, but must never mutate a
    Provider during ACK collection.
    """

    def ensure(
        self,
        candidate: SplitCandidate,
        role_specs: tuple[RoleAssemblySpec, ...],
        *,
        deadline_ms: int,
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


def v3_provider_view_factory(
    verify_offer_signature: Callable[[ProviderOfferV3], bool],
) -> Callable[[Any, str, int, str], ProviderPlanningViewV3]:
    """Convert a signed V3 ACK payload into an immutable planning view.

    V3 offers are graph-wildcarded during the first ACK round because the
    dependency graph is intentionally inspected only after ACK_CLOSED.  The
    factory therefore accepts the actual graph digest as a fourth argument and
    performs the final binding before the strategy sees the view.
    """

    if not callable(verify_offer_signature):
        raise TypeError("V3 Provider offer verifier must be callable")

    def convert(ack: Any, model_intent_digest: str,
                deadline_ms: int, graph_digest: str = "") -> ProviderPlanningViewV3:
        offer = ProviderOfferV3.from_bytes(bytes(ack.payload))
        if bool(getattr(ack, "status", False)) != bool(offer.status):
            raise ValueError("V3 ACK status/offer status mismatch")
        # The offer binds model content, while the public application intent
        # digest binds model+semantics+revision.  The coordinator checks both
        # at the request envelope boundary; V3 offer verification receives the
        # exact request-bound model digest after descriptor inspection.
        return ProviderPlanningViewV3.from_offer(
            offer,
            request_id=str(getattr(ack, "request_id", "") or offer.request_id),
            model_digest=str(model_intent_digest or ""),
            graph_digest=str(graph_digest or ""),
            now_ms=int(time.time() * 1000),
            deadline_ms=int(deadline_ms),
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
        canonical_artifact_ensurer: CanonicalArtifactEnsurer | None = None,
        catalog_snapshot_provider: CatalogSnapshotProvider | None = None,
        budget: CandidateBudget | None = None,
        ack_timeout_ms: int = 300,
        ack_coverage_roles: tuple[str, ...] = (),
        ack_coverage_predicate: Callable[[tuple[Any, ...]], bool] | None = None,
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
        if canonical_artifact_ensurer is not None and not callable(
                getattr(canonical_artifact_ensurer, "ensure", None)):
            raise TypeError(
                "canonical_artifact_ensurer does not implement ensure")
        self.canonical_artifact_ensurer = canonical_artifact_ensurer
        self.catalog_snapshot_provider = (
            catalog_snapshot_provider or (lambda: ()))
        self.budget = budget or CandidateBudget(
            max_candidates=16, max_policy_ms=100)
        self.ack_timeout_ms = int(ack_timeout_ms)
        self.ack_coverage_roles = tuple(str(role) for role in ack_coverage_roles)
        if (len(set(self.ack_coverage_roles)) != len(self.ack_coverage_roles)
                or any(not role for role in self.ack_coverage_roles)):
            raise ValueError("ACK coverage roles must be unique non-empty strings")
        if (ack_coverage_predicate is not None
                and not callable(ack_coverage_predicate)):
            raise TypeError("ack_coverage_predicate must be callable")
        self.ack_coverage_predicate = ack_coverage_predicate

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
        generation_mode: str = "TOKEN_DIAGNOSTIC",
        strategy: ModelPlacementStrategy | None = None,
    ) -> AutomaticInferenceHandle:
        request_started = time.perf_counter()
        timings: dict[str, float] = {}
        if timeout_ms <= self.ack_timeout_ms:
            raise ValueError("request timeout must exceed ACK collection")
        deadline_ms = int(time.time() * 1000) + timeout_ms
        request_id = normalize_request_id_component(
            request_id or ("ndnsf-di-" + uuid.uuid4().hex))
        invocation_id = "invocation:" + canonical_digest({
            "request_id": request_id, "model": model.intent_digest,
        })[7:39]
        # Validate the application/task contract before putting anything on
        # the wire, but deliberately defer model graph inspection and split
        # candidate enumeration.  The generic Request must reach Providers
        # first so their ACK metadata (capacity, cache residency, RTT, and
        # bandwidth) is part of the actual planning input.  This is the
        # request-driven default; PREPLANNED remains the explicit compatibility
        # path in the generic NDNSF API.
        adapter = self._resolve_adapter(task, input, options)
        active_strategy = strategy or self.strategy
        placement_profile = str(
            getattr(active_strategy, "placement_profile", "DI_PLACEMENT_V2"))
        if placement_profile == DI_PLACEMENT_V3:
            return self._request_v3(
                model=model, task=task, input=input, timeout_ms=timeout_ms,
                options=options, objective=objective, constraints=constraints,
                request_id=request_id, generation_mode=generation_mode,
                strategy=active_strategy, adapter=adapter,
            )
        phase_started = time.perf_counter()
        request_payload = self._encode_request(
            model, task, input, options, deadline_ms, request_id,
            self.service_name, invocation_id, generation_mode,
            placement_profile=placement_profile)
        timings["request_encode_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        ack_coverage_predicate = self.ack_coverage_predicate
        if ack_coverage_predicate is None and self.ack_coverage_roles:
            ack_coverage_predicate = AckRoleCoveragePolicy(
                required_roles=self.ack_coverage_roles,
                provider_view_factory=self.provider_view_factory,
                model_intent_digest=model.intent_digest,
                deadline_ms=deadline_ms,
            )
        collaboration_kwargs = dict(
            mode="DEFERRED",
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_id=request_id,
            fail_fast_terminal_selection=True,
        )
        if ack_coverage_predicate is not None:
            collaboration_kwargs["ack_coverage_predicate"] = ack_coverage_predicate
        collaboration = self.service_user.begin_collaboration(
            self.service_name,
            request_payload,
            **collaboration_kwargs,
        )
        print(
            "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
            f"requestId={request_id}",
            "mode=DEFERRED",
            flush=True,
        )
        timings["request_publish_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        closed = collaboration.acks_closed()
        self._validate_ack_closed_binding(closed, request_id)
        print(
            "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
            f"requestId={request_id}",
            f"ackCount={len(closed.candidates)}",
            flush=True,
        )
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

        phase_started = time.perf_counter()
        model_descriptor = adapter.describe_model(
            model.model_name,
            model.content_digest,
            model.semantics_digest,
            source_revision=model.source_revision or "",
        )
        graph = adapter.graph.inspect(model_descriptor)
        if (graph is None
                or not str(getattr(graph, "graph_digest", "")).startswith(
                    "sha256:")):
            raise ValueError(
                "post-ACK planning requires an immutable dependency graph snapshot")
        candidates = adapter.splitter.enumerate_candidates(
            model_descriptor, graph)
        timings["adapter_graph_split_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        if not candidates or len(candidates) > self.budget.max_candidates:
            raise ValueError("adapter returned an invalid candidate set")
        print(
            "NDNSF_DI_AUTOPLANNING_GRAPH_READY",
            f"requestId={request_id}",
            f"graphDigest={graph.graph_digest}",
            f"candidateCount={len(candidates)}",
            "after=ACK_CLOSED",
            flush=True,
        )
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
        print(
            "NDNSF_DI_AUTOPLANNING_PLACEMENT_INPUT",
            f"requestId={request_id}",
            f"catalogCount={len(placement.catalog_snapshot)}",
            f"providerCount={len(placement.providers)}",
            f"candidateCount={len(placement.candidates)}",
            flush=True,
        )
        phase_started = time.perf_counter()
        if active_strategy is None:
            raise ValueError("a placement strategy is required after ACK_CLOSED")
        strategy_identity_digest = canonical_digest({
            "name": active_strategy.name,
            "version": active_strategy.version,
            "state_digest": active_strategy.state_digest,
        })
        decision = evaluate_placement_strategy(active_strategy, placement)
        timings["placement_strategy_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        print(
            "NDNSF_DI_AUTOPLANNING_DECISION",
            f"requestId={request_id}",
            f"preparation={decision.artifact_preparation.value}",
            f"splitId={decision.split_id}",
            f"catalogCount={len(placement.catalog_snapshot)}",
            flush=True,
        )
        if (decision.artifact_preparation is
                ArtifactPreparationMode.REUSE_CACHED
                and not placement.catalog_snapshot):
            # A Provider ACK may describe bytes retained from an older Repo
            # process, but without an ACTIVE catalog this coordinator cannot
            # resolve the content-addressed Data names required by Selection.
            # Fail closed to the first-request publication path instead of
            # turning an unresolvable cache hint into a request timeout.
            evidence = dict(decision.evidence)
            evidence["cache_resolution"] = {
                "status": "UNRESOLVABLE_WITHOUT_ACTIVE_CATALOG",
                "action": "PUBLISH_SELECTED_CANDIDATE",
            }
            decision = replace(
                decision,
                artifact_preparation=ArtifactPreparationMode.GENERATED,
                evidence=evidence,
                evidence_digest=canonical_digest(evidence),
            )
            print(
                "NDNSF_DI_AUTOPLANNING_CACHE_DOWNGRADE",
                f"requestId={request_id}",
                "from=REUSE_CACHED",
                "to=GENERATED",
                "reason=UNRESOLVABLE_WITHOUT_ACTIVE_CATALOG",
                flush=True,
            )
        print(
            "NDNSF_DI_AUTOPLANNING_CANDIDATE_RESOLVE_BEGIN",
            f"requestId={request_id}",
            f"candidateCount={len(candidates)}",
            f"splitId={decision.split_id}",
            flush=True,
        )
        try:
            candidate = next(
                (item for item in candidates
                 if item.candidate_digest == decision.split_id),
                None,
            )
        except Exception as exc:
            print(
                "NDNSF_DI_AUTOPLANNING_CANDIDATE_RESOLVE_FAILED",
                f"requestId={request_id}",
                f"errorType={type(exc).__name__}",
                f"error={exc}",
                flush=True,
            )
            raise
        if candidate is None:
            raise ValueError("placement selected an unknown candidate")
        print(
            "NDNSF_DI_AUTOPLANNING_CANDIDATE_RESOLVE_DONE",
            f"requestId={request_id}",
            f"candidateDigest={candidate.candidate_digest}",
            flush=True,
        )
        print(
            "NDNSF_DI_AUTOPLANNING_ARTIFACTS_BEGIN",
            f"requestId={request_id}",
            f"preparation={decision.artifact_preparation.value}",
            f"materializer={type(self.split_materializer).__module__}.{type(self.split_materializer).__qualname__}",
            f"publisher={type(self.artifact_publisher).__module__}.{type(self.artifact_publisher).__qualname__}",
            flush=True,
        )
        phase_started = time.perf_counter()
        try:
            published = self._prepare_artifacts(
                candidate, decision.artifact_preparation, deadline_ms)
        except Exception as exc:
            print(
                "NDNSF_DI_AUTOPLANNING_ARTIFACTS_FAILED",
                f"requestId={request_id}",
                f"preparation={decision.artifact_preparation.value}",
                f"errorType={type(exc).__name__}",
                f"error={exc}",
                flush=True,
            )
            raise
        print(
            "NDNSF_DI_AUTOPLANNING_ARTIFACTS_READY",
            f"requestId={request_id}",
            f"candidateDigest={candidate.candidate_digest}",
            f"preparation={decision.artifact_preparation.value}",
            flush=True,
        )
        timings["artifact_resolve_publish_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        sealed = self._seal(
            closed.digest, placement, decision, candidate, published,
            invocation_id, strategy_identity_digest,
            generation_mode=generation_mode)
        timings["plan_seal_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        scope_key_data_names = self._publish_scope_keys(
            sealed.key_scopes, deadline_ms)
        sealed = replace(
            sealed,
            scope_key_data_names=scope_key_data_names,
        )
        timings["scope_key_publish_ms"] = (
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
        print(
            "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
            f"requestId={request_id}",
            f"candidateDigest={candidate.candidate_digest}",
            flush=True,
        )
        timings["selection_commit_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        timings["pre_response_setup_total_ms"] = (
            time.perf_counter() - request_started) * 1000.0
        return AutomaticInferenceHandle(
            collaboration, decision, sealed, adapter, timings, invocation_id)

    def _request_v3(
        self,
        *,
        model: ModelRef,
        task: InferenceTaskRef,
        input: ApplicationInput,
        timeout_ms: int,
        options: TaskOptions | None,
        objective: Any,
        constraints: Mapping[str, Any] | None,
        request_id: str,
        generation_mode: str,
        strategy: ModelPlacementStrategy,
        adapter: ModelFamilyAdapter,
    ) -> AutomaticInferenceHandle:
        """Run the real V3 request/ACK/plan/Selection composition.

        V3 never uses the V2 role-split preparation port.  If the adapter
        supplies a canonical ensurer, it is invoked only after ACK_CLOSED and
        graph planning, before Selection is sealed.  Providers still own local
        fetch/assembly/load after Selection; the requester merely publishes or
        resolves the immutable canonical layer identities.
        """

        request_started = time.perf_counter()
        timings: dict[str, float] = {}
        if timeout_ms <= self.ack_timeout_ms:
            raise ValueError("request timeout must exceed ACK collection")
        deadline_ms = int(time.time() * 1000) + int(timeout_ms)
        request_id = normalize_request_id_component(
            request_id or ("ndnsf-di-" + uuid.uuid4().hex))
        invocation_id = "invocation:" + canonical_digest({
            "request_id": request_id, "model": model.intent_digest,
        })[7:39]
        request_payload = self._encode_request(
            model, task, input, options, deadline_ms, request_id,
            self.service_name, invocation_id, generation_mode,
            placement_profile=DI_PLACEMENT_V3)
        ack_coverage_predicate = self.ack_coverage_predicate
        if ack_coverage_predicate is None and self.ack_coverage_roles:
            ack_coverage_predicate = AckRoleCoveragePolicy(
                required_roles=self.ack_coverage_roles,
                provider_view_factory=self.provider_view_factory,
                model_intent_digest=model.intent_digest,
                deadline_ms=deadline_ms,
            )
        collaboration = self.service_user.begin_collaboration(
            self.service_name, request_payload, mode="DEFERRED",
            ack_timeout_ms=self.ack_timeout_ms, timeout_ms=timeout_ms,
            request_id=request_id, fail_fast_terminal_selection=True,
            **({"ack_coverage_predicate": ack_coverage_predicate}
               if ack_coverage_predicate is not None else {}),
        )
        print(
            "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
            f"requestId={request_id}", "mode=DEFERRED", "placement=V3",
            flush=True,
        )
        closed = collaboration.acks_closed()
        self._validate_ack_closed_binding(closed, request_id)
        print(
            "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
            f"requestId={request_id}", f"ackCount={len(closed.candidates)}",
            "placement=V3", flush=True,
        )

        descriptor = adapter.describe_model(
            model.model_name, model.content_digest, model.semantics_digest,
            source_revision=model.source_revision or "",
        )
        graph = adapter.graph.inspect(descriptor)
        if graph is None or not str(getattr(graph, "graph_digest", "")).startswith(
                "sha256:"):
            raise ValueError("V3 planning requires an immutable graph snapshot")
        candidates = tuple(adapter.splitter.enumerate_candidates(descriptor, graph))
        if not candidates or len(candidates) > self.budget.max_candidates:
            raise ValueError("adapter returned an invalid V3 candidate set")

        providers: list[ProviderPlanningViewV3] = []
        for ack in tuple(closed.candidates):
            if not bool(getattr(ack, "status", False)):
                continue
            try:
                view = self.provider_view_factory(
                    ack, model.intent_digest, deadline_ms, graph.graph_digest)
            except TypeError as exc:
                raise TypeError(
                    "V3 provider view factory must accept graph_digest") from exc
            if not isinstance(view, ProviderPlanningViewV3):
                raise TypeError("V3 ACK did not produce ProviderPlanningViewV3")
            providers.append(view)
        if not providers:
            raise ValueError("ACK_CLOSED contains no valid V3 Provider offer")

        # Try graph-derived candidates in a deterministic reuse-first order.
        # The external strategy still controls Provider assignment; candidate
        # identity is bound into the proposal and sealed core.
        def candidate_order(item: SplitCandidate):
            exact = sum(
                1 for role in item.execution_plan.roles
                for view in providers
                if any(proof.role == role and proof.rank == 0
                       and proof.artifact_digest
                       == item.artifacts_by_role[role][0]
                       for proof in view.residency))
            return (
                0 if item.source == SplitSource.PRE_SPLIT else 1,
                -exact,
                canonical_digest(item.estimated_costs),
                item.candidate_digest,
            )

        proposal: PlacementProposalV3 | None = None
        selected_candidate: SplitCandidate | None = None
        candidate_rejections: list[str] = []
        for candidate in sorted(candidates, key=candidate_order):
            role_specs = tuple(self._v3_role_specs(candidate))
            try:
                candidate_proposal = strategy.propose_v3(
                    request_id=collaboration.request_id,
                    attempt=1,
                    model_digest=descriptor.model_digest,
                    graph_digest=graph.graph_digest,
                    roles=role_specs,
                    providers=tuple(providers),
                    ack_closed_digest=closed.digest,
                )
            except (AttributeError, TypeError, ValueError, RuntimeError) as exc:
                # Keep the strategy fail-closed, but do not erase the
                # contract-level reason needed to diagnose a real deployment
                # mismatch.  The message contains only exception type/text;
                # no model bytes or Provider secrets are included.
                candidate_rejections.append(
                    f"{candidate.candidate_digest}:{type(exc).__name__}:{exc}")
                continue
            if not isinstance(candidate_proposal, PlacementProposalV3):
                raise TypeError("V3 strategy returned a non-proposal")
            proposal = replace(
                candidate_proposal,
                candidate_digest=(candidate_proposal.candidate_digest
                                  or candidate.candidate_digest),
            )
            selected_candidate = candidate
            break
        if proposal is None or selected_candidate is None:
            detail = "; ".join(candidate_rejections[-4:])
            raise ValueError(
                "V3 strategy found no feasible graph candidate"
                + (f" ({detail})" if detail else ""))

        # Use the strategy's sealed role specs (including rank/device choices)
        # as the canonical-ensure input; the candidate-derived tuple is only
        # the strategy's initial proposal input.
        role_specs = tuple(proposal.roles)
        canonical_published: PublishedSplit | None = None
        if self.canonical_artifact_ensurer is not None:
            print(
                "NDNSF_DI_CANONICAL_ENSURE_START",
                f"requestId={request_id}",
                f"candidateDigest={selected_candidate.candidate_digest}",
                flush=True,
            )
            canonical_published = self.canonical_artifact_ensurer.ensure(
                selected_candidate, role_specs, deadline_ms=deadline_ms)
            self._validate_published_split(
                selected_candidate, canonical_published)
            print(
                "NDNSF_DI_CANONICAL_ENSURE_DONE",
                f"requestId={request_id}",
                f"candidateDigest={selected_candidate.candidate_digest}",
                flush=True,
            )

        placement_input = PlacementRequest(
            request_id=collaboration.request_id, attempt=1,
            deadline_ms=deadline_ms, model_digest=descriptor.model_digest,
            graph_digest=graph.graph_digest,
            candidate_ids=tuple(item.candidate_digest for item in candidates),
            providers=tuple(providers),
            required_roles=selected_candidate.execution_plan.roles,
            budget=self.budget, objective=objective,
            constraints=dict(constraints or {}),
            catalog_snapshot=tuple(self.catalog_snapshot_provider()),
            task_digest=task.task_descriptor_digest, state_contracts=adapter.state.contracts,
            model=descriptor, graph=graph, candidates=candidates,
        )
        strategy_identity_digest = canonical_digest({
            "name": strategy.name, "version": strategy.version,
            "state_digest": strategy.state_digest,
        })
        core = PlanSealerV3.seal_core(
            {
                "request_id": collaboration.request_id,
                "attempt": 1,
                "now_ms": int(time.time() * 1000),
                "deadline_ms": deadline_ms,
                "ack_closed_digest": closed.digest,
                "candidate_digest": selected_candidate.candidate_digest,
            }, proposal, {view.provider: view for view in providers})
        security_policy_digest = canonical_digest({
            "policy": "ndnsf-di-default-v3",
            "request_id": collaboration.request_id,
            "attempt": 1,
        })
        grants = tuple(
            PlanSealerV3.grant_view(
                core, provider, {item.provider: item for item in providers}[provider],
                security_policy_digest)
            for provider in sorted(set(core.provider_by_role.values()))
        )
        plan_digest = PlanSealerV3.finalize_security(
            core, grants, security_policy_digest)
        provider_views = {item.provider: item for item in providers}

        dependencies: list[CollaborationDependency] = []
        committed_dependencies: list[DIDataDependencyV2] = []
        key_scopes: dict[str, tuple[str, ...]] = {}
        role_scopes: dict[str, list[str]] = {
            role: [] for role in selected_candidate.execution_plan.roles
        }
        input_scopes: dict[str, list[str]] = {
            role: [] for role in selected_candidate.execution_plan.roles
        }
        dependency_dicts: list[dict[str, Any]] = []
        for index, dependency in enumerate(selected_candidate.execution_plan.dependencies):
            scope = f"tensor-{index}-{canonical_digest(dependency)[7:23]}"
            dependency_dict = {
                "producers": [dependency.producer],
                "consumers": [dependency.consumer],
                "key_scope": scope,
                "topic_prefix": "/activation",
                "required": True,
                "tensors": list(dependency.tensor_edges),
            }
            dependency_dicts.append(dependency_dict)
            dependencies.append(CollaborationDependency(
                producers=[dependency.producer], consumers=[dependency.consumer],
                key_scope=scope, topic_prefix="/activation", required=True,
            ))
            committed_dependencies.append(DIDataDependencyV2(
                producers=(dependency.producer,), consumers=(dependency.consumer,),
                key_scope=scope, topic_prefix="/activation", required=True,
                tensors=tuple(dependency.tensor_edges),
            ))
            key_scopes[scope] = (dependency.producer, dependency.consumer)
            role_scopes[dependency.producer].append(scope)
            role_scopes[dependency.consumer].append(scope)
            input_scopes[dependency.consumer].append(scope)
        if str(generation_mode).upper() == "FULL":
            key_scopes[GENERATION_CONTROL_SCOPE] = tuple(
                selected_candidate.execution_plan.roles)
            for role in selected_candidate.execution_plan.roles:
                role_scopes[role].append(GENERATION_CONTROL_SCOPE)

        role_specs_by_name = {item.role: item for item in proposal.roles}
        artifact_names: dict[str, str] = {}
        roles: list[CollaborationRole] = []
        for role in selected_candidate.execution_plan.roles:
            spec = role_specs_by_name[role]
            artifact_name = (
                canonical_published.artifact_data_names_by_role[role]
                if canonical_published is not None
                else self._v3_artifact_name(model, graph, spec, adapter))
            artifact_names[role] = artifact_name
            roles.append(CollaborationRole(
                role=role, service=self.service_name, artifact=artifact_name,
                allow_dynamic_provisioning=False,
            ))

        assignment_payloads: dict[str, bytes] = {}
        providers_by_role = dict(proposal.provider_by_role)
        for provider in sorted(set(providers_by_role.values())):
            provider_roles = tuple(
                spec for spec in proposal.roles
                if providers_by_role[
                    spec.role if sum(item.role == spec.role for item in proposal.roles) == 1
                    else f"{spec.role}#{spec.rank}"] == provider
            )
            projection = ProviderSelectionProjectionV3(
                provider=provider, request_id=collaboration.request_id,
                attempt=1, plan_core_digest=core.plan_core_digest or core.digest(),
                plan_digest=plan_digest, roles=provider_roles,
                dependencies=tuple(dependency_dicts), deadline_ms=deadline_ms,
            )
            payload = projection.to_bytes()
            role_counts = {
                item.role: sum(other.role == item.role for other in proposal.roles)
                for item in provider_roles
            }
            for spec in provider_roles:
                # ``provider_by_role`` uses a stable role#rank key whenever
                # one logical role has multiple ranks.  Keep the assignment
                # payload map in that same key space; using ``spec.role``
                # here silently overwrote rank 0 with rank 1 and left the
                # sealed plan with an incomplete per-role projection.
                key = (spec.role if role_counts[spec.role] == 1
                       else f"{spec.role}#{spec.rank}")
                assignment_payloads[key] = payload

        scope_key_data_names = self._publish_scope_keys(key_scopes, deadline_ms)
        sealed = SealedCollaborationPlan(
            ack_closed_digest=closed.digest,
            placement_input_digest=placement_input.digest(),
            placement_decision_digest=proposal.digest(),
            strategy_identity_digest=strategy_identity_digest,
            execution_policy=DATA_DRIVEN_V2, roles=tuple(roles),
            dependencies=tuple(dependencies), key_scopes=key_scopes,
            role_scopes={key: tuple(value) for key, value in role_scopes.items()},
            providers_by_role=providers_by_role,
            artifact_data_names=artifact_names,
            scope_key_data_names=scope_key_data_names,
            assignment_payloads_by_role=assignment_payloads,
        )

        assignments = []
        exact_all = True
        for spec in proposal.roles:
            key = (spec.role if sum(item.role == spec.role for item in proposal.roles) == 1
                   else f"{spec.role}#{spec.rank}")
            provider = providers_by_role[key]
            view = provider_views[provider]
            requirement = selected_candidate.requirements_by_role[spec.role]
            required_mib = int(
                math.ceil((requirement.estimated_peak_gpu_memory_bytes or 0) / (1024 * 1024)))
            device = spec.device_set[0] if len(spec.device_set) == 1 else (
                "cpu" if is_cpu_backend(spec.backend) else "")
            assignments.append(ProviderAssignment(
                role=spec.role, provider=provider,
                required_gpu_memory_mb=required_mib,
                backend=spec.backend, device=device,
            ))
            exact = any(
                proof.role == spec.role and proof.rank == spec.rank
                and proof.artifact_digest == spec.artifact_digest
                for proof in view.residency)
            exact_all = exact_all and exact
        decision = PlacementDecision(
            split_id=selected_candidate.candidate_digest,
            split_digest=selected_candidate.candidate_digest,
            assignments=tuple(assignments), fallback_order={},
            input_digest=placement_input.digest(),
            evidence_digest=canonical_digest({
                "placement": placement_input.digest(),
                "proposal": proposal.digest(), "core": core.digest(),
                "plan": plan_digest,
            }),
            artifact_preparation=(ArtifactPreparationMode.REUSE_CACHED
                                  if exact_all else ArtifactPreparationMode.GENERATED),
            evidence={"placementProfile": DI_PLACEMENT_V3,
                      "planCoreDigest": core.plan_core_digest or core.digest(),
                      "planDigest": plan_digest},
        )
        self._publish_scope_keys  # keep the trusted side-effect boundary explicit
        committed = collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=list(sealed.roles), key_scopes={
                key: list(value) for key, value in sealed.key_scopes.items()},
            dependencies=list(sealed.dependencies),
            artifact_data_names=dict(sealed.artifact_data_names),
            scope_key_data_names=dict(sealed.scope_key_data_names),
            role_scopes={key: list(value) for key, value in sealed.role_scopes.items()},
            role_provider_assignments=dict(sealed.providers_by_role),
            assignment_payloads_by_role=dict(sealed.assignment_payloads_by_role),
        )
        if not committed:
            raise RuntimeError(
                "V3 collaboration plan commit produced no Selection")
        print(
            "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
            f"requestId={request_id}",
            f"candidateDigest={selected_candidate.candidate_digest}",
            "placement=V3", f"planDigest={plan_digest}", flush=True,
        )
        timings["pre_response_setup_total_ms"] = (
            time.perf_counter() - request_started) * 1000.0
        return AutomaticInferenceHandle(
            collaboration, decision, sealed, adapter, timings, invocation_id)

    @staticmethod
    def _v3_role_specs(candidate: SplitCandidate) -> tuple[RoleAssemblySpec, ...]:
        def role_kind(role: str) -> str:
            """Map a Collaboration role to its adapter-defined identity kind.

            The role name is an execution namespace (and may contain many
            slash components); it is never safe to place it in one canonical
            NDN component.  The semantic kind is stable across stage labels
            and therefore preserves canonical-layer reuse.
            """
            parts = tuple(part.lower() for part in str(role).strip("/").split("/")
                          if part)
            if any("tensor" in part or "shard" in part or "rank" in part
                   for part in parts):
                return "TENSOR_RANK"
            if any(part in {"pipeline", "stage", "stages"}
                       or part.startswith("stage-")
                   for part in parts):
                return "PIPELINE_RANGE"
            # A non-layer adapter still has a deterministic component-set
            # identity; it must not fall back to the raw role path.
            return "COMPONENT_SET"

        specs = []
        for role in candidate.execution_plan.roles:
            requirement = candidate.requirements_by_role[role]
            begin, end = AutomaticPlanningCoordinator._role_layer_range(candidate, role)
            if begin is None or end is None:
                # Non-layer adapters still need a deterministic assembly range.
                owned = [
                    node for node, owner in candidate.execution_plan.node_roles.items()
                    if owner == role
                ]
                begin = 0
                end = max(1, len(owned))
            recipe_digest = canonical_digest({
                "candidate": candidate.candidate_digest,
                "role": role, "begin": begin, "end": end,
                "backends": requirement.backends,
            })
            specs.append(RoleAssemblySpec(
                role=role, rank=0, layer_begin=int(begin), layer_end=int(end),
                recipe_digest=recipe_digest,
                artifact_digest=candidate.artifacts_by_role[role][0],
                backend=str(requirement.backends[0]),
                adapter_id=str(candidate.model.adapter.name),
                adapter_version=str(candidate.model.adapter.version),
                role_kind=role_kind(role),
            ))
        return tuple(specs)

    @staticmethod
    def _v3_artifact_name(
        model: ModelRef, graph: Any, spec: RoleAssemblySpec,
        adapter: ModelFamilyAdapter,
    ) -> str:
        """Build the stable Repo identity carried in generic Role.artifact."""

        from .canonical_artifacts import canonical_layer_name

        return canonical_layer_name(
            publisher="/ndnsf-di", model_name=model.model_name,
            model_digest=model.content_digest, profile=adapter.descriptor.name,
            graph_digest=str(graph.graph_digest), role_kind=spec.role_kind,
            layer_begin=spec.layer_begin, layer_end=spec.layer_end,
            rank=spec.rank, recipe_digest=spec.recipe_digest,
            object_digest=spec.artifact_digest,
        )

    def request_application(
        self,
        *,
        model: ModelRef,
        input: GenerationInput,
        generation: GenerationConfig,
        strategy: ModelPlacementStrategy | None = None,
        request_id: str = "",
    ) -> AutomaticInferenceHandle:
        """Encode a public generation call without accepting deployment data."""
        if not isinstance(model, ModelRef):
            raise TypeError("model must be ModelRef")
        if not model.source_revision:
            raise ValueError(
                "public model requests require an immutable model revision")
        if not isinstance(input, GenerationInput):
            raise TypeError("input must be GenerationInput")
        if not isinstance(generation, GenerationConfig):
            raise TypeError("generation must be GenerationConfig")
        if generation.adapter_name:
            adapter = self.adapters.get(generation.adapter_name)
            if adapter is None:
                raise ValueError("requested model adapter is not allowlisted")
        elif len(self.adapters) == 1:
            adapter = next(iter(self.adapters.values()))
        else:
            raise ValueError(
                "generation.adapter_name is required when multiple adapters exist")
        task = InferenceTaskRef.from_adapter(adapter)
        encoded = adapter.task.encode_input(
            input.to_task_value(), generation.to_task_options())
        return self.request(
            model=model,
            task=task,
            input=encoded,
            timeout_ms=generation.timeout_ms,
            objective=None,
            constraints={},
            request_id=request_id,
            generation_mode="FULL",
            strategy=strategy,
        )

    @staticmethod
    def _validate_ack_closed_binding(closed: Any, request_id: str) -> None:
        """Reject an ACK snapshot that is not bound to this invocation.

        Generic NDNSF authenticates and freezes the ACK_CLOSED snapshot.  This
        NDNSF-DI boundary additionally verifies the inference request binding
        before graph inspection or an external placement strategy can run.
        A late ACK cannot alter the frozen tuple; a foreign-request ACK must
        never become placement input.
        """
        if str(getattr(closed, "request_id", "")) != request_id:
            raise ValueError("ACK_CLOSED request binding mismatch")
        for candidate in tuple(getattr(closed, "candidates", ())):
            if str(getattr(candidate, "request_id", "")) != request_id:
                raise ValueError("ACK candidate request binding mismatch")

    def generate(self, request: GenerationRequest) -> AutomaticInferenceHandle:
        """Submit one full-generation request through one durable invocation."""
        if not isinstance(request, GenerationRequest):
            raise TypeError("generate requires a GenerationRequest")
        return self.request(
            model=request.model,
            task=request.task,
            input=request.input,
            timeout_ms=request.timeout_ms,
            options=request.options,
            objective=request.objective,
            constraints=request.constraints,
            request_id=request.request_id,
            generation_mode=request.output_mode,
        )

    def _publish_scope_keys(
        self,
        key_scopes: Mapping[str, tuple[str, ...]],
        deadline_ms: int,
    ) -> dict[str, str]:
        """Publish one request-scoped encryption key for every data edge.

        The generic collaboration wire contract carries both the symbolic
        ``key_scopes`` and the encrypted Data names that let each Provider
        retrieve its key.  Automatic planning used to populate only the
        former, which made a correctly sealed dependency graph fail at the
        first inter-stage fetch.  Keep publication in the trusted coordinator
        after ACK/placement and before Selection commit so the committed plan
        is self-contained and providers never have to guess or share keys.
        """
        data_names: dict[str, str] = {}
        for scope in key_scopes:
            remaining_ms = int(deadline_ms - int(time.time() * 1000))
            if remaining_ms <= 0:
                raise TimeoutError("scope-key publication deadline expired")
            result = self.service_user.publish_encrypted_large_data(
                self.service_name,
                secrets.token_bytes(32),
                object_label=f"inference-scope-key-{scope}",
                freshness_ms=max(60000, remaining_ms),
            )
            if not result.success:
                raise RuntimeError(
                    f"scope key publish failed for {scope}: {result.error}")
            data_name = str(result.encrypted_data_name)
            if not data_name.startswith("/"):
                raise ValueError(
                    f"scope key publication returned a non-absolute Data name: {scope}")
            data_names[str(scope)] = data_name
        return data_names

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
        if preparation in {
                ArtifactPreparationMode.PRE_SPLIT,
                ArtifactPreparationMode.REUSE_CACHED,
        }:
            # REUSE_CACHED is deliberately resolve-only: ACK cache evidence
            # says the selected Providers already hold these content-addressed
            # shards, so a second invocation must not materialize or publish
            # the model again.
            published = self.artifact_publisher.resolve_existing(
                candidate, deadline_ms=deadline_ms)
        else:
            print(
                "NDNSF_DI_AUTOPLANNING_MATERIALIZE_START",
                f"candidateDigest={candidate.candidate_digest}",
                f"materializer={type(self.split_materializer).__module__}.{type(self.split_materializer).__qualname__}",
                flush=True,
            )
            materialized = self.split_materializer.materialize(
                candidate, deadline_ms=deadline_ms)
            print(
                "NDNSF_DI_AUTOPLANNING_MATERIALIZE_DONE",
                f"candidateDigest={candidate.candidate_digest}",
                flush=True,
            )
            expected = {
                role: candidate.artifacts_by_role[role][0]
                for role in candidate.execution_plan.roles
            }
            if (materialized.candidate_digest != candidate.candidate_digest
                    or dict(materialized.artifact_digests_by_role) != expected):
                raise ValueError(
                    "materialized split does not match the selected candidate")
            print(
                "NDNSF_DI_AUTOPLANNING_PUBLISH_START",
                f"candidateDigest={candidate.candidate_digest}",
                f"publisher={type(self.artifact_publisher).__module__}.{type(self.artifact_publisher).__qualname__}",
                flush=True,
            )
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
        generation_mode: str = "TOKEN_DIAGNOSTIC",
        placement_profile: str = "DI_PLACEMENT_V2",
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
                "generation_mode": str(generation_mode),
                "placement_profile": str(placement_profile),
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
        strategy_identity_digest: str,
        generation_mode: str = "TOKEN_DIAGNOSTIC",
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
        committed_dependencies: list[DIDataDependencyV2] = []
        key_scopes: dict[str, tuple[str, ...]] = {}
        role_scopes: dict[str, list[str]] = {
            role: [] for role in candidate.execution_plan.roles
        }
        input_scopes_by_role: dict[str, list[str]] = {
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
            committed_dependencies.append(DIDataDependencyV2(
                producers=(dependency.producer,),
                consumers=(dependency.consumer,),
                key_scope=scope,
                topic_prefix="/activation",
                required=True,
                tensors=tuple(dependency.tensor_edges),
            ))
            key_scopes[scope] = (
                dependency.producer, dependency.consumer)
            role_scopes[dependency.producer].append(scope)
            role_scopes[dependency.consumer].append(scope)
            input_scopes_by_role[dependency.consumer].append(scope)
        if str(generation_mode).upper() == "FULL":
            # Full generation uses a bounded provider-to-provider token
            # control loop.  This scope is not a graph edge and therefore does
            # not change model splitting or topological graph identity.  All
            # selected roles receive it explicitly in the sealed plan so the
            # loop remains encrypted and request-scoped.
            key_scopes[GENERATION_CONTROL_SCOPE] = tuple(
                candidate.execution_plan.roles)
            for role in candidate.execution_plan.roles:
                role_scopes[role].append(GENERATION_CONTROL_SCOPE)
        plan_digest = canonical_digest({
            "ack_closed_digest": ack_closed_digest,
            "placement_input_digest": placement.digest(),
            "placement_decision_digest": decision.digest(),
            "strategy_identity_digest": strategy_identity_digest,
            "execution_policy": DATA_DRIVEN_V2,
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
            dependencies_by_role = {
                role: tuple(
                    dependency for dependency in committed_dependencies
                    if role in dependency.producers + dependency.consumers)
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
                    dependency_digest=canonical_digest(
                        dependencies_by_role[role]),
                    adapter_id=candidate.model.adapter.name,
                    adapter_version=candidate.model.adapter.version,
                    dependencies=dependencies_by_role[role],
                    required_gpu_mib=assignments[
                        role].required_gpu_memory_mb,
                    backend=assignments[role].backend,
                    device=self._resolve_execution_device(
                        assignments[role], view),
                    input_grant_digests=(canonical_digest({
                        "role": role,
                        "request_id": placement.request_id,
                        "attempt": placement.attempt,
                    }),),
                    required_input_scopes=tuple(
                        input_scopes_by_role[role]),
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
                execution_policy=DATA_DRIVEN_V2,
            ).to_bytes()
            for role in provider_roles:
                assignment_payloads[role] = provider_assignment
        return SealedCollaborationPlan(
            ack_closed_digest=ack_closed_digest,
            placement_input_digest=placement.digest(),
            placement_decision_digest=decision.digest(),
            strategy_identity_digest=strategy_identity_digest,
            execution_policy=DATA_DRIVEN_V2,
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
    def _resolve_execution_device(
        assignment: ProviderAssignment,
        view: ProviderPlanningView,
    ) -> str:
        """Bind an external strategy decision to one signed ACK device."""

        if assignment.backend not in view.backends:
            raise ValueError(
                f"Provider {view.provider} did not offer backend "
                f"{assignment.backend}")
        device = assignment.device
        if not device:
            if is_cpu_backend(assignment.backend):
                device = "cpu"
            elif len(view.devices) == 1:
                device = view.devices[0]
            else:
                raise ValueError(
                    f"Provider {view.provider} assignment requires one exact "
                    "device from the signed ACK offer")
        if device == "cpu":
            if not is_cpu_backend(assignment.backend):
                raise ValueError("non-CPU backend cannot be assigned to CPU")
        elif device not in view.devices:
            raise ValueError(
                f"Provider {view.provider} did not offer device {device}")
        return device

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
    "CanonicalArtifactEnsurer",
    "InferenceTaskRef",
    "ModelRef",
    "TaskOptions",
    "replan_placement_request",
]
