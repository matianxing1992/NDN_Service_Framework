"""Standard output objects for model splitters and deployment planners.

NDNSF-DistributedInference accepts this output regardless of whether it came
from an ONNX analyzer, a PyTorch/model-specific splitter, a handwritten
application planner, or a future optimizer.
"""

from __future__ import annotations

from dataclasses import dataclass, field, fields, is_dataclass
from enum import Enum
import hashlib
import json
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping

from .plan import InferenceDependency


def _canonical_bytes(value: Any) -> bytes:
    def plain(item: Any) -> Any:
        if isinstance(item, Enum):
            return item.value
        if isinstance(item, Mapping):
            return {str(key): plain(value) for key, value in item.items()}
        if is_dataclass(item):
            return {
                value.name: plain(getattr(item, value.name))
                for value in fields(item)
            }
        if isinstance(item, (tuple, list)):
            return [plain(value) for value in item]
        return item

    return json.dumps(
        plain(value), sort_keys=True, separators=(",", ":"), allow_nan=False,
    ).encode("utf-8")


def _digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(_canonical_bytes(value)).hexdigest()


def canonical_contract_digest(value: Any) -> str:
    """Return the deterministic digest used by common adapter contracts."""

    return _digest(value)


def _frozen_mapping(value: Mapping[str, Any]) -> Mapping[str, Any]:
    return MappingProxyType({
        str(key): item for key, item in value.items()
    })


def _require_digest(value: str, name: str) -> None:
    if not isinstance(value, str) or len(value) != 71 or not value.startswith("sha256:"):
        raise ValueError(f"{name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(f"{name} must be a canonical sha256 digest") from exc


@dataclass(frozen=True)
class AdapterDescriptor:
    name: str
    version: str
    state_digest: str
    abi: str
    model_formats: tuple[str, ...]
    tasks: tuple[str, ...]
    backends: tuple[str, ...]
    precisions: tuple[str, ...]
    input_schema_digest: str
    options_schema_digest: str
    result_schema_digest: str
    graph_schema_digest: str
    split_schema_digest: str
    state_schema_digest: str
    graph_inspectable: bool
    splittable: bool
    deterministic_analysis: bool = True

    def __post_init__(self) -> None:
        if (not self.name or not self.version or not self.abi
                or not self.model_formats or not self.tasks or not self.backends
                or not self.precisions):
            raise ValueError("model adapter descriptor is incomplete")
        for name in (
                "state_digest", "input_schema_digest", "options_schema_digest",
                "result_schema_digest", "graph_schema_digest",
                "split_schema_digest", "state_schema_digest"):
            _require_digest(getattr(self, name), name)

    @property
    def descriptor_digest(self) -> str:
        return _digest(self)


@dataclass(frozen=True)
class ModelDescriptor:
    model_name: str
    content_digest: str
    semantics_digest: str
    graph_digest: str
    model_format: str
    precision: str
    adapter: AdapterDescriptor
    source_revision: str = ""

    def __post_init__(self) -> None:
        if (not self.model_name or self.model_format not in self.adapter.model_formats
                or self.precision not in self.adapter.precisions):
            raise ValueError("model descriptor is incompatible with its adapter")
        for name in ("content_digest", "semantics_digest", "graph_digest"):
            _require_digest(getattr(self, name), name)

    def validate_graph(self, graph: "ModelGraphSnapshot") -> None:
        if graph.graph_digest != self.graph_digest:
            raise ValueError("model and graph digest mismatch")
        if graph.adapter.descriptor_digest != self.adapter.descriptor_digest:
            raise ValueError("model and graph adapter mismatch")

    @property
    def model_digest(self) -> str:
        return _digest(self)


@dataclass(frozen=True)
class GraphNodeView:
    node_id: str
    operation: str

    def __post_init__(self) -> None:
        if not self.node_id or not self.operation:
            raise ValueError("invalid graph node")


@dataclass(frozen=True)
class TensorContract:
    name: str
    dtype: str
    shape: tuple[int | str, ...]
    estimated_bytes: int | None

    def __post_init__(self) -> None:
        if (not self.name or not self.dtype or self.estimated_bytes is not None
                and self.estimated_bytes < 0):
            raise ValueError("invalid tensor contract")


@dataclass(frozen=True)
class TensorEdgeView:
    edge_id: str
    producer: str
    consumers: tuple[str, ...]
    dtype: str
    shape: tuple[int | str, ...]
    estimated_bytes: int | None

    def __post_init__(self) -> None:
        if (not self.edge_id or not self.producer or not self.consumers
                or len(set(self.consumers)) != len(self.consumers)
                or not self.dtype or self.estimated_bytes is not None
                and self.estimated_bytes < 0):
            raise ValueError("invalid tensor edge")


@dataclass(frozen=True)
class ModelGraphSnapshot:
    graph_digest: str
    adapter: AdapterDescriptor
    nodes: tuple[GraphNodeView, ...]
    edges: tuple[TensorEdgeView, ...]
    topological_order: tuple[str, ...]
    legal_cut_edges: tuple[str, ...]
    model_inputs: tuple[TensorContract, ...]
    model_outputs: tuple[TensorContract, ...]

    def __post_init__(self) -> None:
        _require_digest(self.graph_digest, "graph_digest")
        node_ids = tuple(item.node_id for item in self.nodes)
        if (not node_ids or len(set(node_ids)) != len(node_ids)
                or set(self.topological_order) != set(node_ids)
                or len(self.topological_order) != len(node_ids)):
            raise ValueError("graph topological order is incomplete")
        order = {node: index for index, node in enumerate(self.topological_order)}
        edge_ids = tuple(edge.edge_id for edge in self.edges)
        if len(set(edge_ids)) != len(edge_ids):
            raise ValueError("duplicate tensor edge")
        for edge in self.edges:
            if edge.producer not in order or any(item not in order for item in edge.consumers):
                raise ValueError("tensor edge references an unknown graph node")
            if any(order[edge.producer] >= order[item] for item in edge.consumers):
                raise ValueError("graph topological order is not acyclic")
        if not set(self.legal_cut_edges).issubset(set(edge_ids)):
            raise ValueError("legal cut references an unknown tensor edge")


@dataclass(frozen=True)
class SplitterDescriptor:
    name: str
    version: str
    state_digest: str
    deterministic: bool = True

    def __post_init__(self) -> None:
        if not self.name or not self.version:
            raise ValueError("splitter descriptor is incomplete")
        _require_digest(self.state_digest, "splitter state_digest")

    @property
    def descriptor_digest(self) -> str:
        return _digest(self)


@dataclass(frozen=True)
class RoleDependency:
    producer: str
    consumer: str
    tensor_edges: tuple[str, ...]

    def __post_init__(self) -> None:
        if (not self.producer or not self.consumer
                or self.producer == self.consumer or not self.tensor_edges):
            raise ValueError("invalid role dependency")


@dataclass(frozen=True)
class RoleExecutionPlan:
    roles: tuple[str, ...]
    dependencies: tuple[RoleDependency, ...]
    node_roles: Mapping[str, str]

    def __post_init__(self) -> None:
        object.__setattr__(self, "roles", tuple(self.roles))
        object.__setattr__(self, "dependencies", tuple(self.dependencies))
        object.__setattr__(self, "node_roles", _frozen_mapping(self.node_roles))
        roles = set(self.roles)
        if (not roles or len(roles) != len(self.roles)
                or set(self.node_roles.values()) != roles):
            raise ValueError("role execution plan is incomplete")
        incoming = {role: 0 for role in roles}
        outgoing = {role: [] for role in roles}
        for edge in self.dependencies:
            if edge.producer not in roles or edge.consumer not in roles:
                raise ValueError("role dependency references an unknown role")
            incoming[edge.consumer] += 1
            outgoing[edge.producer].append(edge.consumer)
        ready = sorted(role for role, count in incoming.items() if count == 0)
        visited = []
        while ready:
            role = ready.pop(0)
            visited.append(role)
            for consumer in sorted(outgoing[role]):
                incoming[consumer] -= 1
                if incoming[consumer] == 0:
                    ready.append(consumer)
                    ready.sort()
        if len(visited) != len(roles):
            raise ValueError("role dependency graph must be acyclic")


@dataclass(frozen=True)
class RoleResourceRequirement:
    backends: tuple[str, ...]
    weight_bytes: int | None
    workspace_bytes: int | None
    kv_bytes: int | None
    activation_bytes: int | None
    transient_bytes: int | None
    safety_margin: float = 1.1

    def __post_init__(self) -> None:
        values = (
            self.weight_bytes, self.workspace_bytes, self.kv_bytes,
            self.activation_bytes, self.transient_bytes,
        )
        if (not self.backends or any(item is not None and item < 0 for item in values)
                or self.safety_margin < 1.0):
            raise ValueError("invalid role resource requirement")

    @property
    def estimated_peak_gpu_memory_bytes(self) -> int | None:
        values = (
            self.weight_bytes, self.workspace_bytes, self.kv_bytes,
            self.activation_bytes, self.transient_bytes,
        )
        if any(item is None for item in values):
            return None
        return int(sum(values) * self.safety_margin)


class SplitSource(str, Enum):
    PRE_SPLIT = "PRE_SPLIT"
    GENERATED = "GENERATED"


@dataclass(frozen=True)
class SplitCandidate:
    source: SplitSource | str
    splitter: SplitterDescriptor
    model: ModelDescriptor
    graph_digest: str
    execution_plan: RoleExecutionPlan
    fragments_by_role: Mapping[str, str]
    artifacts_by_role: Mapping[str, tuple[str, ...]]
    requirements_by_role: Mapping[str, RoleResourceRequirement]
    cross_partition_tensors: tuple[str, ...]
    estimated_costs: Mapping[str, int | float | None]

    def __post_init__(self) -> None:
        object.__setattr__(self, "source", SplitSource(self.source))
        object.__setattr__(
            self, "fragments_by_role", _frozen_mapping(self.fragments_by_role))
        object.__setattr__(
            self, "artifacts_by_role",
            _frozen_mapping({
                role: tuple(values)
                for role, values in self.artifacts_by_role.items()
            }),
        )
        object.__setattr__(
            self, "requirements_by_role",
            _frozen_mapping(self.requirements_by_role),
        )
        object.__setattr__(
            self, "cross_partition_tensors",
            tuple(self.cross_partition_tensors),
        )
        object.__setattr__(
            self, "estimated_costs", _frozen_mapping(self.estimated_costs))
        _require_digest(self.graph_digest, "candidate graph_digest")
        roles = set(self.execution_plan.roles)
        if (set(self.fragments_by_role) != roles
                or set(self.artifacts_by_role) != roles
                or set(self.requirements_by_role) != roles):
            raise ValueError("split candidate does not cover every role")
        for value in self.fragments_by_role.values():
            _require_digest(value, "fragment digest")
        for values in self.artifacts_by_role.values():
            if not values:
                raise ValueError("split candidate role has no artifact")
            for value in values:
                _require_digest(value, "artifact digest")
        if len(set(self.cross_partition_tensors)) != len(
                self.cross_partition_tensors):
            raise ValueError("split candidate duplicates a tensor edge")

    @property
    def candidate_digest(self) -> str:
        return _digest(self)

    def validate_against(self, graph: ModelGraphSnapshot) -> None:
        self.model.validate_graph(graph)
        if self.graph_digest != graph.graph_digest:
            raise ValueError("split candidate graph digest mismatch")
        if set(self.execution_plan.node_roles) != {
                item.node_id for item in graph.nodes}:
            raise ValueError("split candidate does not partition every graph node")
        cuts = set(self.cross_partition_tensors)
        if not cuts.issubset(set(graph.legal_cut_edges)):
            raise ValueError("split candidate crosses an illegal tensor edge")
        dependency_tensors = {
            tensor
            for dependency in self.execution_plan.dependencies
            for tensor in dependency.tensor_edges
        }
        if dependency_tensors != cuts:
            raise ValueError("split candidate dependency tensors are incomplete")


def _append_unique(mapping: dict[str, list[Any]], key: str, value: Any) -> None:
    values = mapping.setdefault(key, [])
    if value not in values:
        values.append(value)


def authorization_summary(config: dict[str, Any]) -> dict[str, Any]:
    """Return a deployment-review summary derived from service permissions."""

    user_services: dict[str, list[Any]] = {}
    provider_services: dict[str, list[Any]] = {}
    for service in config.get("services", []):
        if not isinstance(service, dict):
            continue
        service_name = str(service.get("name", ""))
        if not service_name:
            continue
        for user in service.get("users", []) or []:
            _append_unique(user_services, str(user), service_name)
        for provider in service.get("providers", []) or []:
            if not isinstance(provider, dict):
                continue
            identity = str(provider.get("identity", ""))
            if not identity:
                continue
            _append_unique(provider_services, identity, {
                "service": service_name,
                "roles": provider.get("roles", []),
            })
    return {
        "users": [
            {"identity": identity, "services": services}
            for identity, services in user_services.items()
        ],
        "providers": [
            {"identity": identity, "services": services}
            for identity, services in provider_services.items()
        ],
    }


@dataclass(frozen=True)
class SplitArtifact:
    """One artifact produced by a model splitter."""

    role: str
    path: str
    artifact_name: str
    filename: str = ""
    kind: str = "model"
    backend: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)

    def resolved_filename(self) -> str:
        return self.filename or Path(self.path).name


@dataclass(frozen=True)
class SplitServiceSpec:
    """A service layout emitted by a splitter.

    The service name identifies exactly one model layout. Different splits of
    the same model should be represented as different services.
    """

    name: str
    model_name: str
    roles: list[str]
    dependencies: list[InferenceDependency]
    artifacts: list[SplitArtifact] = field(default_factory=list)
    input_schema: dict[str, Any] = field(default_factory=dict)
    output_schema: dict[str, Any] = field(default_factory=dict)
    users: list[str] = field(default_factory=list)
    providers: list[dict[str, Any]] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_policy_service(
        self,
        *,
        users: list[str],
        providers: list[dict[str, Any]],
    ) -> dict[str, Any]:
        return {
            "name": self.name,
            "model": self.model_name,
            "users": list(self.users or users),
            "providers": list(self.providers or providers),
            "roles": list(self.roles),
            "dependencies": [
                {
                    "producers": list(dep.producers),
                    "consumers": list(dep.consumers),
                    "key_scope": dep.key_scope,
                    "topic_prefix": dep.topic_prefix,
                    "required": dep.required,
                    **({"tensors": list(dep.tensors)} if dep.tensors else {}),
                    **({"object_name_template": dep.object_name_template}
                       if dep.object_name_template else {}),
                    **({"expected_segments": dep.expected_segments}
                       if dep.expected_segments else {}),
                    **({"expected_bytes": dep.expected_bytes}
                       if dep.expected_bytes else {}),
                }
                for dep in self.dependencies
            ],
            "artifacts": [
                {
                    "role": artifact.role,
                    "path": artifact.path,
                    "artifact": artifact.artifact_name,
                    "filename": artifact.resolved_filename(),
                    "kind": artifact.kind,
                    "backend": artifact.backend,
                    "metadata": dict(artifact.metadata),
                }
                for artifact in self.artifacts
            ],
            "input": dict(self.input_schema),
            "output": dict(self.output_schema),
            "metadata": dict(self.metadata),
        }

    def artifact_for_role(self, role: str) -> SplitArtifact:
        for artifact in self.artifacts:
            if artifact.role == role:
                return artifact
        raise KeyError(f"split service {self.name} has no artifact for role {role}")


@dataclass(frozen=True)
class SplitterOutput:
    """Complete deployment-facing output from a model splitter."""

    application: str
    controller: str
    group: str
    user: str
    provider_prefix: str
    services: list[SplitServiceSpec]
    provider_identities: list[str] = field(default_factory=list)
    trust_app_roots: list[str] = field(default_factory=list)
    trust_anchor_file: str = ""
    artifact_allowlist: list[str] = field(default_factory=list)
    artifact_sandbox: dict[str, Any] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    def service(self, name: str) -> SplitServiceSpec:
        for service in self.services:
            if service.name == name:
                return service
        raise KeyError(f"splitter output has no service {name}")

    def to_policy_config(self) -> dict[str, Any]:
        provider_identities = list(self.provider_identities) or [
            self.provider_prefix,
            self.provider_prefix.rstrip("/") + "/A",
            self.provider_prefix.rstrip("/") + "/B",
            self.provider_prefix.rstrip("/") + "/C",
        ]
        providers = [
            {"identity": identity, "roles": "all"}
            for identity in provider_identities
        ]
        return {
            "application": self.application,
            "controller": self.controller,
            "group": self.group,
            "runtime": {
                "user_identity": self.user,
                "provider_prefix": self.provider_prefix,
            },
            "trust": {
                "app_roots": list(self.trust_app_roots),
            },
            "artifact_security": {
                **({"anchor_file": self.trust_anchor_file}
                   if self.trust_anchor_file else {}),
                "allowlist": list(self.artifact_allowlist),
                "sandbox": dict(self.artifact_sandbox),
            },
            "services": [
                service.to_policy_service(
                    users=[self.user],
                    providers=providers,
                )
                for service in self.services
            ],
        }

    def write_policy_config(self, path: str | Path) -> None:
        """Write the generated service policy YAML.

        The dependency graph in this file is splitter output, not handwritten
        NDNSF logic.
        """

        try:
            import yaml  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "Writing splitter policy YAML requires PyYAML"
            ) from exc
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        config = self.to_policy_config()
        editable_keys = (
            "application",
            "controller",
            "group",
            "runtime",
            "trust",
            "artifact_security",
        )
        editable_section = {
            key: config[key]
            for key in editable_keys
            if key in config
        }
        generated_section = {
            "services": config.get("services", []),
        }
        text = (
            "# Generated by NDNSF-DistributedInference.\n"
            "# editable deployment section\n"
            "# Edit these fields when moving this deployment to a new\n"
            "# namespace, controller, trust root, runtime identity, or\n"
            "# artifact security policy. runtime.user_identity must also\n"
            "# appear in at least one service users list below.\n\n"
            + yaml.safe_dump(editable_section, sort_keys=False)
            + "\n"
            "# generated authorization summary\n"
            "# Read-only review aid derived from services[].users/providers.\n"
            "# Do not treat this as a second permission source; edit exact\n"
            "# service users/providers in the model-plan section below.\n\n"
            + yaml.safe_dump(
                {"authorization_summary": authorization_summary(config)},
                sort_keys=False,
            )
            + "\n"
            "# generated model-plan section\n"
            "# For each service, edit only exact users/providers for deployment.\n"
            "# roles/dependencies/artifacts/input/output are splitter or planner\n"
            "# output; regenerate this section when the model split changes.\n\n"
            + yaml.safe_dump(generated_section, sort_keys=False)
        )
        target.write_text(
            text,
            encoding="utf-8",
        )
