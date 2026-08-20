"""Qwen3.6 placement adapter for the three-stage RTX validation profile."""

from __future__ import annotations

from dataclasses import dataclass
import json
from typing import Any, Mapping

from ..base import (
    AdapterPortDescriptor,
    ApplicationInput,
    InferenceStateClass,
    InferenceTaskDescriptor,
    ModelFamilyAdapter,
)
from ..builtin import (
    GuardedRunnerAdapter,
    StaticGraphAdapter,
    StaticStateAdapter,
    _state_contract,
)
from ...splitter import (
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


QWEN36_27B_MODEL = "Qwen/Qwen3.6-27B"
QWEN36_27B_REVISION = "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9"
QWEN36_27B_LAYER_RANGES = ((0, 21), (21, 42), (42, 64))
QWEN36_STAGE_ROLES = tuple(
    f"/LLM/Pipeline/Stage/{index}" for index in range(3)
)


def _digest(label: str) -> str:
    return canonical_contract_digest({"qwen36_placement": label})


def _normalize_digest(value: str) -> str:
    value = str(value)
    return value if value.startswith("sha256:") else "sha256:" + value


@dataclass(frozen=True)
class BytesGenerationTaskAdapter:
    port_descriptor: AdapterPortDescriptor
    descriptor: InferenceTaskDescriptor

    def encode_input(
        self, value: Any, options: Mapping[str, Any],
    ) -> ApplicationInput:
        if not isinstance(value, (bytes, bytearray, memoryview)):
            raise TypeError("Qwen generation input must be encoded pipeline bytes")
        return ApplicationInput(
            task_name=self.descriptor.task_name,
            input_schema_digest=self.descriptor.input_schema_digest,
            options_schema_digest=self.descriptor.options_schema_digest,
            payload=bytes(value),
            options=json.dumps(
                dict(options), sort_keys=True, separators=(",", ":"),
            ).encode(),
            metadata={"adapter": "qwen36-27b-pipeline"},
        )

    def decode_result(self, payload: bytes) -> bytes:
        return bytes(payload)


@dataclass(frozen=True)
class Qwen36ThreeStageSplitter:
    descriptor: AdapterPortDescriptor
    splitter_descriptor: SplitterDescriptor
    artifact_digests_by_role: Mapping[str, str]
    weight_bytes_by_role: Mapping[str, int]

    def enumerate_candidates(
        self, model: ModelDescriptor, graph: ModelGraphSnapshot,
    ) -> tuple[SplitCandidate, ...]:
        model.validate_graph(graph)
        node_roles: dict[str, str] = {"embedding": QWEN36_STAGE_ROLES[0]}
        for layer in range(64):
            role_index = next(
                index for index, (start, end) in enumerate(
                    QWEN36_27B_LAYER_RANGES)
                if start <= layer < end
            )
            node_roles[f"layer-{layer:02d}"] = QWEN36_STAGE_ROLES[role_index]
        node_roles["final-norm-head"] = QWEN36_STAGE_ROLES[-1]
        cut_edges = (
            "hidden-layer-20-to-21",
            "hidden-layer-41-to-42",
        )
        dependencies = (
            RoleDependency(
                QWEN36_STAGE_ROLES[0], QWEN36_STAGE_ROLES[1],
                (cut_edges[0],),
            ),
            RoleDependency(
                QWEN36_STAGE_ROLES[1], QWEN36_STAGE_ROLES[2],
                (cut_edges[1],),
            ),
        )
        artifacts = {
            role: (_normalize_digest(self.artifact_digests_by_role[role]),)
            for role in QWEN36_STAGE_ROLES
        }
        requirements = {
            role: RoleResourceRequirement(
                backends=("transformers", "cuda"),
                weight_bytes=int(self.weight_bytes_by_role[role]),
                workspace_bytes=1024 * 1024 * 1024,
                kv_bytes=0,
                activation_bytes=512 * 1024 * 1024,
                transient_bytes=512 * 1024 * 1024,
                safety_margin=1.10,
            )
            for role in QWEN36_STAGE_ROLES
        }
        candidate = SplitCandidate(
            source=SplitSource.PRE_SPLIT,
            splitter=self.splitter_descriptor,
            model=model,
            graph_digest=graph.graph_digest,
            execution_plan=RoleExecutionPlan(
                roles=QWEN36_STAGE_ROLES,
                dependencies=dependencies,
                node_roles=node_roles,
            ),
            fragments_by_role={
                role: values[0] for role, values in artifacts.items()
            },
            artifacts_by_role=artifacts,
            requirements_by_role=requirements,
            cross_partition_tensors=cut_edges,
            estimated_costs={
                "role_count": 3,
                "decoder_layers": 64,
                "known_transfer_bytes": 0,
                "unknown_transfer_tensors": 2,
            },
        )
        candidate.validate_against(graph)
        return (candidate,)


def build_qwen36_27b_three_stage_adapter(
    *,
    artifact_digests_by_role: Mapping[str, str],
    weight_bytes_by_role: Mapping[str, int],
) -> ModelFamilyAdapter:
    """Build the content-bound adapter used by Spec 162.

    The graph is a dependency graph, not a hard-coded provider assignment.
    Provider placement still happens only after ACK_CLOSED.
    """
    if (set(artifact_digests_by_role) != set(QWEN36_STAGE_ROLES)
            or set(weight_bytes_by_role) != set(QWEN36_STAGE_ROLES)):
        raise ValueError("Qwen3.6 adapter requires all three exact stage artifacts")

    input_schema = _digest("pipeline-input-v1")
    options_schema = _digest("generation-options-v1")
    result_schema = _digest("pipeline-result-v1")
    descriptor = AdapterDescriptor(
        name="qwen36-27b-pipeline",
        version="1.0.0",
        state_digest=_digest("adapter-state"),
        abi="python-v1",
        model_formats=("transformers-stage-package",),
        tasks=("text-generation",),
        backends=("transformers", "cuda"),
        precisions=("bfloat16",),
        input_schema_digest=input_schema,
        options_schema_digest=options_schema,
        result_schema_digest=result_schema,
        graph_schema_digest=_digest("decoder-dependency-graph-v1"),
        split_schema_digest=_digest("three-stage-split-v1"),
        state_schema_digest=_digest("request-and-exact-prefix-state-v1"),
        graph_inspectable=True,
        splittable=True,
    )
    node_ids = (
        ("embedding",)
        + tuple(f"layer-{index:02d}" for index in range(64))
        + ("final-norm-head",)
    )
    edges = []
    for index in range(len(node_ids) - 1):
        producer = node_ids[index]
        consumer = node_ids[index + 1]
        if producer == "embedding":
            edge_id = "hidden-embedding-to-layer-00"
        elif consumer == "final-norm-head":
            edge_id = "hidden-layer-63-to-final"
        else:
            left = int(producer.split("-")[1])
            right = int(consumer.split("-")[1])
            edge_id = f"hidden-layer-{left}-to-{right}"
        edges.append(TensorEdgeView(
            edge_id=edge_id,
            producer=producer,
            consumers=(consumer,),
            dtype="bfloat16",
            shape=("batch", "sequence", "hidden"),
            estimated_bytes=None,
        ))
    graph_digest = canonical_contract_digest({
        "model": QWEN36_27B_MODEL,
        "revision": QWEN36_27B_REVISION,
        "nodes": node_ids,
        "edges": tuple(edge.edge_id for edge in edges),
        "legal_cuts": tuple(edge.edge_id for edge in edges),
    })
    graph = ModelGraphSnapshot(
        graph_digest=graph_digest,
        adapter=descriptor,
        nodes=tuple(GraphNodeView(node, node) for node in node_ids),
        edges=tuple(edges),
        topological_order=node_ids,
        legal_cut_edges=tuple(edge.edge_id for edge in edges),
        model_inputs=(TensorContract(
            "token-ids", "int64", ("batch", "sequence"), None),),
        model_outputs=(TensorContract(
            "logits", "bfloat16", ("batch", "sequence", "vocabulary"), None),),
    )
    graph_port = StaticGraphAdapter(
        AdapterPortDescriptor(
            "qwen36-graph", "1", _digest("graph-port")),
        graph,
    )
    splitter_port = AdapterPortDescriptor(
        "qwen36-three-stage-splitter", "1", _digest("splitter-port"))
    splitter = Qwen36ThreeStageSplitter(
        splitter_port,
        SplitterDescriptor(
            splitter_port.name, splitter_port.version,
            splitter_port.state_digest),
        dict(artifact_digests_by_role),
        {role: int(value) for role, value in weight_bytes_by_role.items()},
    )
    task = BytesGenerationTaskAdapter(
        AdapterPortDescriptor(
            "qwen36-generation-task", "1", _digest("task-port")),
        InferenceTaskDescriptor(
            "text-generation", input_schema, options_schema, result_schema),
    )
    state = StaticStateAdapter(
        AdapterPortDescriptor(
            "qwen36-state", "1", _digest("state-port")),
        (
            _state_contract(
                "qwen36", InferenceStateClass.REQUEST_SCOPED, retention_ms=0),
            _state_contract(
                "qwen36", InferenceStateClass.EXACT_PREFIX_REUSABLE,
                retention_ms=300_000),
        ),
    )
    runner = GuardedRunnerAdapter(
        AdapterPortDescriptor(
            "qwen36-runner", "1", _digest("runner-port")))
    return ModelFamilyAdapter(
        descriptor, graph_port, splitter, task, state, runner)


__all__ = [
    "QWEN36_27B_LAYER_RANGES",
    "QWEN36_27B_MODEL",
    "QWEN36_27B_REVISION",
    "QWEN36_STAGE_ROLES",
    "build_qwen36_27b_three_stage_adapter",
]
