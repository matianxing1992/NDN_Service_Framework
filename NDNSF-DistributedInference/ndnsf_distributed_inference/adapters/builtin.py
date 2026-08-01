"""Reference adapters proving the common NDNSF-DI carrier is model-neutral."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .base import (
    AdapterPortDescriptor,
    InferenceStateClass,
    InferenceStateContract,
    InferenceTaskDescriptor,
    JsonTaskAdapter,
    ModelFamilyAdapter,
)
from ..splitter import (
    AdapterDescriptor,
    GraphNodeView,
    ModelDescriptor,
    ModelGraphSnapshot,
    RoleDependency,
    RoleExecutionPlan,
    RoleResourceRequirement,
    SplitCandidate,
    SplitSource,
    SplitterDescriptor,
    TensorContract,
    TensorEdgeView,
    canonical_contract_digest,
)


def _named_digest(name: str) -> str:
    return canonical_contract_digest({"ndnsf_di_fixture": name})


@dataclass(frozen=True)
class StaticGraphAdapter:
    descriptor: AdapterPortDescriptor
    snapshot: ModelGraphSnapshot

    @property
    def graph_digest(self) -> str:
        return self.snapshot.graph_digest

    def inspect(self, model: ModelDescriptor) -> ModelGraphSnapshot:
        model.validate_graph(self.snapshot)
        return self.snapshot


@dataclass(frozen=True)
class StaticStateAdapter:
    descriptor: AdapterPortDescriptor
    contracts: tuple[InferenceStateContract, ...]


@dataclass(frozen=True)
class GuardedRunnerAdapter:
    descriptor: AdapterPortDescriptor
    requires_accepted_selection: bool = True

    def create(
        self,
        *,
        accepted_selection_digest: str,
        role: str,
        artifacts: tuple[str, ...],
    ) -> dict[str, Any]:
        if not accepted_selection_digest:
            raise ValueError("runner creation requires an accepted Selection")
        if not role or not artifacts:
            raise ValueError("runner creation requires one accepted role and artifacts")
        return {
            "selection_digest": accepted_selection_digest,
            "role": role,
            "artifacts": tuple(artifacts),
            "runner_descriptor_digest": self.descriptor.descriptor_digest,
        }


@dataclass(frozen=True)
class SequentialFixtureSplitter:
    descriptor: AdapterPortDescriptor
    splitter_descriptor: SplitterDescriptor
    per_role_kv_bytes: int

    def enumerate_candidates(
        self,
        model: ModelDescriptor,
        graph: ModelGraphSnapshot,
    ) -> tuple[SplitCandidate, ...]:
        model.validate_graph(graph)
        if len(graph.nodes) == 1:
            roles = ("atomic-0",)
            node_roles = {graph.nodes[0].node_id: roles[0]}
            dependencies: tuple[RoleDependency, ...] = ()
            cross_tensors: tuple[str, ...] = ()
        else:
            midpoint = max(1, len(graph.nodes) // 2)
            roles = ("stage-0", "stage-1")
            node_roles = {
                node.node_id: roles[0 if index < midpoint else 1]
                for index, node in enumerate(graph.nodes)
            }
            cross_tensors = tuple(
                edge.edge_id for edge in graph.edges
                if any(node_roles[item] != node_roles[edge.producer]
                       for item in edge.consumers)
            )
            dependencies = (
                RoleDependency(roles[0], roles[1], cross_tensors),
            ) if cross_tensors else ()

        fragments = {
            role: canonical_contract_digest({
                "model": model.model_digest,
                "graph": graph.graph_digest,
                "role": role,
                "nodes": sorted(
                    node for node, assigned in node_roles.items()
                    if assigned == role
                ),
            })
            for role in roles
        }
        requirements = {
            role: RoleResourceRequirement(
                backends=model.adapter.backends,
                weight_bytes=1024,
                workspace_bytes=256,
                kv_bytes=self.per_role_kv_bytes,
                activation_bytes=sum(
                    edge.estimated_bytes or 0 for edge in graph.edges
                    if edge.edge_id in cross_tensors
                ),
                transient_bytes=128,
            )
            for role in roles
        }
        candidate = SplitCandidate(
            source=SplitSource.GENERATED,
            splitter=self.splitter_descriptor,
            model=model,
            graph_digest=graph.graph_digest,
            execution_plan=RoleExecutionPlan(
                roles=roles,
                dependencies=dependencies,
                node_roles=node_roles,
            ),
            fragments_by_role=fragments,
            artifacts_by_role={
                role: (fragment,) for role, fragment in fragments.items()
            },
            requirements_by_role=requirements,
            cross_partition_tensors=cross_tensors,
            estimated_costs={
                "role_count": len(roles),
                "known_transfer_bytes": sum(
                    edge.estimated_bytes or 0 for edge in graph.edges
                    if edge.edge_id in cross_tensors
                ),
                "unknown_transfer_tensors": sum(
                    edge.estimated_bytes is None for edge in graph.edges
                    if edge.edge_id in cross_tensors
                ),
            },
        )
        candidate.validate_against(graph)
        return (candidate,)


def _state_contract(
    family: str,
    state_class: InferenceStateClass,
    *,
    retention_ms: int,
) -> InferenceStateContract:
    reusable = state_class in (
        InferenceStateClass.SESSION_SCOPED,
        InferenceStateClass.EXACT_PREFIX_REUSABLE,
        InferenceStateClass.CUSTOM_ADAPTER_MANAGED,
    )
    return InferenceStateContract(
        profile=(
            "EXACT_PREFIX_KV_V1"
            if state_class is InferenceStateClass.EXACT_PREFIX_REUSABLE
            else f"{family.upper()}_{state_class.value}_V1"
        ),
        state_class=state_class,
        identity_schema_digest=_named_digest(f"{family}-state-identity"),
        estimator_schema_digest=_named_digest(f"{family}-state-estimator"),
        allowed_tiers=("GPU", "HOST_RAM") if reusable else ("REQUEST_LOCAL",),
        owner_scope="requester-security-domain",
        role_scope="assigned-role",
        confidentiality="recipient-confidential",
        maximum_retention_ms=retention_ms,
        eviction_policy="bounded-lru" if reusable else "terminal-destroy",
        boot_epoch_bound=reusable,
        cache_epoch_bound=reusable,
        pin_required_for_reuse=reusable,
        migration_supported=False,
        revalidation_rule="exact-digest-epoch-expiry-and-access-domain",
        cleanup_rule="release-grants-pins-and-request-scoped-state",
    )


def _build_adapter(
    *,
    family: str,
    task_name: str,
    model_format: str,
    graph_inspectable: bool,
    splittable: bool,
    operations: tuple[str, ...],
    tensor_dtype: str,
    state_contracts: tuple[InferenceStateContract, ...],
    per_role_kv_bytes: int,
) -> ModelFamilyAdapter:
    input_schema = _named_digest(f"{family}-input-schema")
    options_schema = _named_digest(f"{family}-options-schema")
    result_schema = _named_digest(f"{family}-result-schema")
    abi = "python-v1"
    descriptor = AdapterDescriptor(
        name=family,
        version="1.0.0",
        state_digest=_named_digest(f"{family}-adapter-state"),
        abi=abi,
        model_formats=(model_format,),
        tasks=(task_name,),
        backends=("cpu", "cuda"),
        precisions=("float32",),
        input_schema_digest=input_schema,
        options_schema_digest=options_schema,
        result_schema_digest=result_schema,
        graph_schema_digest=_named_digest("common-model-graph-v1"),
        split_schema_digest=_named_digest("common-split-candidate-v1"),
        state_schema_digest=_named_digest("common-inference-state-v1"),
        graph_inspectable=graph_inspectable,
        splittable=splittable,
    )
    node_ids = tuple(f"node-{index}" for index in range(len(operations)))
    edges = tuple(
        TensorEdgeView(
            edge_id=f"tensor-{index}",
            producer=node_ids[index],
            consumers=(node_ids[index + 1],),
            dtype=tensor_dtype,
            shape=("batch", "features"),
            estimated_bytes=4096,
        )
        for index in range(len(node_ids) - 1)
    )
    graph_digest = canonical_contract_digest({
        "family": family,
        "nodes": tuple(zip(node_ids, operations)),
        "edges": tuple(
            (edge.edge_id, edge.producer, edge.consumers, edge.dtype, edge.shape)
            for edge in edges
        ),
    })
    graph = ModelGraphSnapshot(
        graph_digest=graph_digest,
        adapter=descriptor,
        nodes=tuple(
            GraphNodeView(node_id, operation)
            for node_id, operation in zip(node_ids, operations)
        ),
        edges=edges,
        topological_order=node_ids,
        legal_cut_edges=tuple(edge.edge_id for edge in edges) if splittable else (),
        model_inputs=(
            TensorContract("application-input", tensor_dtype, ("batch",), None),
        ),
        model_outputs=(
            TensorContract("application-result", tensor_dtype, ("batch",), None),
        ),
    )
    graph_port = StaticGraphAdapter(
        AdapterPortDescriptor(
            f"{family}-graph", "1", _named_digest(f"{family}-graph-port"), abi,
        ),
        graph,
    )
    split_port_descriptor = AdapterPortDescriptor(
        f"{family}-splitter", "1", _named_digest(f"{family}-split-port"), abi,
    )
    splitter = SequentialFixtureSplitter(
        split_port_descriptor,
        SplitterDescriptor(
            name=split_port_descriptor.name,
            version=split_port_descriptor.version,
            state_digest=split_port_descriptor.state_digest,
        ),
        per_role_kv_bytes,
    )
    task = JsonTaskAdapter(
        AdapterPortDescriptor(
            f"{family}-task", "1", _named_digest(f"{family}-task-port"), abi,
        ),
        InferenceTaskDescriptor(
            task_name, input_schema, options_schema, result_schema,
        ),
    )
    state = StaticStateAdapter(
        AdapterPortDescriptor(
            f"{family}-state", "1", _named_digest(f"{family}-state-port"), abi,
        ),
        state_contracts,
    )
    runner = GuardedRunnerAdapter(
        AdapterPortDescriptor(
            f"{family}-runner", "1", _named_digest(f"{family}-runner-port"), abi,
        ),
    )
    return ModelFamilyAdapter(
        descriptor, graph_port, splitter, task, state, runner,
    )


def build_llm_text_adapter() -> ModelFamilyAdapter:
    return _build_adapter(
        family="llm-text",
        task_name="text-generation",
        model_format="onnx",
        graph_inspectable=True,
        splittable=True,
        operations=("embedding-and-blocks", "normalization-and-output"),
        tensor_dtype="float32",
        state_contracts=(
            _state_contract(
                "llm-text", InferenceStateClass.REQUEST_SCOPED, retention_ms=0,
            ),
            _state_contract(
                "llm-text", InferenceStateClass.EXACT_PREFIX_REUSABLE,
                retention_ms=300_000,
            ),
        ),
        per_role_kv_bytes=2048,
    )


def build_object_detection_adapter() -> ModelFamilyAdapter:
    return _build_adapter(
        family="object-detection",
        task_name="object-detection",
        model_format="onnx",
        graph_inspectable=True,
        splittable=True,
        operations=("feature-extraction", "detection-head"),
        tensor_dtype="float32",
        state_contracts=(
            _state_contract(
                "object-detection", InferenceStateClass.STATELESS, retention_ms=0,
            ),
        ),
        per_role_kv_bytes=0,
    )


def build_opaque_container_adapter() -> ModelFamilyAdapter:
    return _build_adapter(
        family="opaque-container",
        task_name="opaque-inference",
        model_format="oci",
        graph_inspectable=False,
        splittable=False,
        operations=("opaque-atomic-execution",),
        tensor_dtype="bytes",
        state_contracts=(
            _state_contract(
                "opaque-container", InferenceStateClass.REQUEST_SCOPED,
                retention_ms=0,
            ),
        ),
        per_role_kv_bytes=0,
    )
