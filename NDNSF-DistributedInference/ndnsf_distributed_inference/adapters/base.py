"""Model-family-neutral adapter contracts for NDNSF-DI.

The planner consumes only the pure graph, split, task, and state ports.  The
runner port is a separate trusted post-Selection capability and cannot alter a
validated plan.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
import json
from typing import Any, Mapping, Protocol

from ..splitter import (
    AdapterDescriptor,
    ModelDescriptor,
    ModelGraphSnapshot,
    SplitCandidate,
    canonical_contract_digest,
)


def _require_digest(value: str, name: str) -> None:
    if not isinstance(value, str) or len(value) != 71 or not value.startswith(
            "sha256:"):
        raise ValueError(f"{name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(f"{name} must be a canonical sha256 digest") from exc


@dataclass(frozen=True)
class AdapterPortDescriptor:
    name: str
    version: str
    state_digest: str
    abi: str = "python-v1"

    def __post_init__(self) -> None:
        if not self.name or not self.version or not self.abi:
            raise ValueError("adapter port descriptor is incomplete")
        _require_digest(self.state_digest, "adapter port state_digest")

    @property
    def descriptor_digest(self) -> str:
        return canonical_contract_digest(self)


@dataclass(frozen=True)
class InferenceTaskDescriptor:
    task_name: str
    input_schema_digest: str
    options_schema_digest: str
    result_schema_digest: str

    def __post_init__(self) -> None:
        if not self.task_name:
            raise ValueError("inference task name is required")
        for name in (
                "input_schema_digest", "options_schema_digest",
                "result_schema_digest"):
            _require_digest(getattr(self, name), name)


@dataclass(frozen=True)
class ApplicationInput:
    """Canonical adapter output carried by the generic inference request."""

    task_name: str
    input_schema_digest: str
    options_schema_digest: str
    payload: bytes
    options: bytes
    metadata: Mapping[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not self.task_name or not isinstance(self.payload, bytes) or not isinstance(
                self.options, bytes):
            raise ValueError("application input is incomplete")
        _require_digest(self.input_schema_digest, "input_schema_digest")
        _require_digest(self.options_schema_digest, "options_schema_digest")
        object.__setattr__(
            self, "metadata",
            {str(key): str(value) for key, value in self.metadata.items()},
        )


class InferenceStateClass(str, Enum):
    STATELESS = "STATELESS"
    REQUEST_SCOPED = "REQUEST_SCOPED"
    SESSION_SCOPED = "SESSION_SCOPED"
    EXACT_PREFIX_REUSABLE = "EXACT_PREFIX_REUSABLE"
    CUSTOM_ADAPTER_MANAGED = "CUSTOM_ADAPTER_MANAGED"


@dataclass(frozen=True)
class InferenceStateContract:
    profile: str
    state_class: InferenceStateClass | str
    identity_schema_digest: str
    estimator_schema_digest: str
    allowed_tiers: tuple[str, ...]
    owner_scope: str
    role_scope: str
    confidentiality: str
    maximum_retention_ms: int
    eviction_policy: str
    boot_epoch_bound: bool
    cache_epoch_bound: bool
    pin_required_for_reuse: bool
    migration_supported: bool
    revalidation_rule: str
    cleanup_rule: str
    cross_security_domain: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(self, "state_class", InferenceStateClass(self.state_class))
        if (not self.profile or not self.allowed_tiers or not self.owner_scope
                or not self.role_scope or not self.confidentiality
                or self.maximum_retention_ms < 0 or not self.eviction_policy
                or not self.revalidation_rule or not self.cleanup_rule):
            raise ValueError("inference state contract is incomplete")
        for name in ("identity_schema_digest", "estimator_schema_digest"):
            _require_digest(getattr(self, name), name)
        if (self.state_class in (
                InferenceStateClass.STATELESS,
                InferenceStateClass.REQUEST_SCOPED)
                and self.maximum_retention_ms != 0):
            raise ValueError("non-reusable state cannot have retention TTL")
        if self.cross_security_domain:
            raise ValueError("cross-security-domain state reuse is not permitted")


class GraphAdapter(Protocol):
    descriptor: AdapterPortDescriptor
    graph_digest: str

    def inspect(self, model: ModelDescriptor) -> ModelGraphSnapshot:
        ...


class ModelSplitter(Protocol):
    descriptor: AdapterPortDescriptor

    def enumerate_candidates(
        self,
        model: ModelDescriptor,
        graph: ModelGraphSnapshot,
    ) -> tuple[SplitCandidate, ...]:
        ...


class TaskAdapter(Protocol):
    port_descriptor: AdapterPortDescriptor
    descriptor: InferenceTaskDescriptor

    def encode_input(
        self,
        value: Any,
        options: Mapping[str, Any],
    ) -> ApplicationInput:
        ...

    def decode_result(self, payload: bytes) -> Any:
        ...


class StateAdapter(Protocol):
    descriptor: AdapterPortDescriptor
    contracts: tuple[InferenceStateContract, ...]


class RunnerAdapter(Protocol):
    descriptor: AdapterPortDescriptor
    requires_accepted_selection: bool

    def create(
        self,
        *,
        accepted_selection_digest: str,
        role: str,
        artifacts: tuple[str, ...],
    ) -> Any:
        ...


@dataclass(frozen=True)
class ModelFamilyAdapter:
    descriptor: AdapterDescriptor
    graph: GraphAdapter
    splitter: ModelSplitter
    task: TaskAdapter
    state: StateAdapter
    runner: RunnerAdapter

    def __post_init__(self) -> None:
        if self.task.descriptor.task_name not in self.descriptor.tasks:
            raise ValueError("task is not declared by model adapter")
        bindings = (
            ("input_schema_digest", self.task.descriptor.input_schema_digest),
            ("options_schema_digest", self.task.descriptor.options_schema_digest),
            ("result_schema_digest", self.task.descriptor.result_schema_digest),
        )
        for name, value in bindings:
            if getattr(self.descriptor, name) != value:
                raise ValueError(f"adapter {name} binding mismatch")
        if self.runner.descriptor.abi != self.descriptor.abi:
            raise ValueError("runner ABI does not match model adapter")
        if not self.runner.requires_accepted_selection:
            raise ValueError("runner must require an accepted Selection")
        if not self.state.contracts:
            raise ValueError("adapter must explicitly declare its state contract")

    def describe_model(
        self,
        model_name: str,
        content_digest: str,
        semantics_digest: str,
        *,
        source_revision: str = "",
    ) -> ModelDescriptor:
        return ModelDescriptor(
            model_name=model_name,
            content_digest=content_digest,
            semantics_digest=semantics_digest,
            graph_digest=self.graph.graph_digest,
            model_format=self.descriptor.model_formats[0],
            precision=self.descriptor.precisions[0],
            adapter=self.descriptor,
            source_revision=source_revision,
        )

    @property
    def composition_digest(self) -> str:
        return self.recompute_composition_digest()

    def recompute_composition_digest(self) -> str:
        return canonical_contract_digest({
            "adapter": self.descriptor.descriptor_digest,
            "graph": self.graph.descriptor.descriptor_digest,
            "splitter": self.splitter.descriptor.descriptor_digest,
            "task": self.task.port_descriptor.descriptor_digest,
            "state": self.state.descriptor.descriptor_digest,
            "runner": self.runner.descriptor.descriptor_digest,
        })

    def validate_pin(
        self,
        *,
        adapter_descriptor_digest: str,
        composition_digest: str,
    ) -> None:
        _require_digest(adapter_descriptor_digest, "adapter_descriptor_digest")
        _require_digest(composition_digest, "composition_digest")
        if adapter_descriptor_digest != self.descriptor.descriptor_digest:
            raise ValueError("model adapter descriptor pin mismatch")
        if composition_digest != self.recompute_composition_digest():
            raise ValueError("model adapter composition pin mismatch")


class JsonTaskAdapter:
    """Small schema-pinned JSON task port used by built-in adapter fixtures."""

    def __init__(
        self,
        port_descriptor: AdapterPortDescriptor,
        descriptor: InferenceTaskDescriptor,
    ) -> None:
        self.port_descriptor = port_descriptor
        self.descriptor = descriptor

    def encode_input(
        self,
        value: Any,
        options: Mapping[str, Any],
    ) -> ApplicationInput:
        payload = json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False,
        ).encode("utf-8")
        encoded_options = json.dumps(
            dict(options), sort_keys=True, separators=(",", ":"), allow_nan=False,
        ).encode("utf-8")
        return ApplicationInput(
            task_name=self.descriptor.task_name,
            input_schema_digest=self.descriptor.input_schema_digest,
            options_schema_digest=self.descriptor.options_schema_digest,
            payload=payload,
            options=encoded_options,
        )

    def decode_result(self, payload: bytes) -> Any:
        return json.loads(bytes(payload).decode("utf-8"))
