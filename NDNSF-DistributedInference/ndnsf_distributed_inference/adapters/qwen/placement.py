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
    adapter_name: str = "qwen-three-stage-pipeline"

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
            metadata={"adapter": self.adapter_name},
        )

    def decode_result(self, payload: bytes) -> bytes:
        return bytes(payload)


@dataclass(frozen=True)
class QwenThreeStageSplitter:
    descriptor: AdapterPortDescriptor
    splitter_descriptor: SplitterDescriptor
    artifact_digests_by_role: Mapping[str, str]
    weight_bytes_by_role: Mapping[str, int]
    layer_ranges: tuple[tuple[int, int], ...]
    roles: tuple[str, ...] = QWEN36_STAGE_ROLES

    def __post_init__(self) -> None:
        ranges = tuple((int(start), int(end))
                       for start, end in self.layer_ranges)
        if (len(ranges) != len(self.roles) or not ranges
                or ranges[0][0] != 0
                or any(start < 0 or end <= start for start, end in ranges)
                or any(ranges[index][1] != ranges[index + 1][0]
                       for index in range(len(ranges) - 1))
                or set(self.artifact_digests_by_role) != set(self.roles)
                or set(self.weight_bytes_by_role) != set(self.roles)):
            raise ValueError("Qwen stage ranges and artifacts are inconsistent")
        object.__setattr__(self, "layer_ranges", ranges)

    def enumerate_candidates(
        self, model: ModelDescriptor, graph: ModelGraphSnapshot,
    ) -> tuple[SplitCandidate, ...]:
        model.validate_graph(graph)
        node_roles: dict[str, str] = {"embedding": self.roles[0]}
        decoder_layers = self.layer_ranges[-1][1]
        for layer in range(decoder_layers):
            role_index = next(
                index for index, (start, end) in enumerate(self.layer_ranges)
                if start <= layer < end
            )
            node_roles[f"layer-{layer:02d}"] = self.roles[role_index]
        node_roles["final-norm-head"] = self.roles[-1]
        cut_edges = tuple(
            f"hidden-layer-{end - 1}-to-{end}"
            for _start, end in self.layer_ranges[:-1]
        )
        dependencies = tuple(
            RoleDependency(
                self.roles[index], self.roles[index + 1],
                (cut_edges[index],),
            )
            for index in range(len(self.roles) - 1)
        )
        artifacts = {
            role: (_normalize_digest(self.artifact_digests_by_role[role]),)
            for role in self.roles
        }
        requirements = {
            role: RoleResourceRequirement(
                backends=("transformers", "cuda", "transformers-cpu"),
                weight_bytes=int(self.weight_bytes_by_role[role]),
                workspace_bytes=1024 * 1024 * 1024,
                kv_bytes=0,
                activation_bytes=512 * 1024 * 1024,
                transient_bytes=512 * 1024 * 1024,
                safety_margin=1.10,
            )
            for role in self.roles
        }
        candidate = SplitCandidate(
            source=SplitSource.PRE_SPLIT,
            splitter=self.splitter_descriptor,
            model=model,
            graph_digest=graph.graph_digest,
            execution_plan=RoleExecutionPlan(
                roles=self.roles,
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
                "role_count": len(self.roles),
                "decoder_layers": decoder_layers,
                "known_transfer_bytes": 0,
                "unknown_transfer_tensors": 2,
            },
        )
        candidate.validate_against(graph)
        return (candidate,)


def build_qwen_three_stage_adapter(
    *,
    model_name: str,
    revision: str,
    layer_ranges: tuple[tuple[int, int], ...],
    artifact_digests_by_role: Mapping[str, str],
    weight_bytes_by_role: Mapping[str, int],
    precision: str = "bfloat16",
    adapter_name: str = "qwen-three-stage-pipeline",
) -> ModelFamilyAdapter:
    """Build one content-bound Qwen dependency graph from model metadata.

    The graph is a dependency graph, not a hard-coded provider assignment.
    Provider placement still happens only after ACK_CLOSED.
    """
    if (set(artifact_digests_by_role) != set(QWEN36_STAGE_ROLES)
            or set(weight_bytes_by_role) != set(QWEN36_STAGE_ROLES)
            or not model_name or not revision or not precision):
        raise ValueError("Qwen adapter requires one exact three-stage model")
    ranges = tuple((int(start), int(end)) for start, end in layer_ranges)
    if (len(ranges) != 3 or ranges[0][0] != 0
            or any(start < 0 or end <= start for start, end in ranges)
            or any(ranges[index][1] != ranges[index + 1][0]
                   for index in range(2))):
        raise ValueError("Qwen adapter requires three contiguous layer ranges")
    decoder_layers = ranges[-1][1]

    input_schema = _digest("pipeline-input-v1")
    options_schema = _digest("generation-options-v1")
    result_schema = _digest("pipeline-result-v1")
    descriptor = AdapterDescriptor(
        name=adapter_name,
        version="1.0.0",
        state_digest=_digest("adapter-state"),
        abi="python-v1",
        model_formats=("transformers-stage-package",),
        tasks=("text-generation",),
        backends=("transformers", "cuda", "transformers-cpu"),
        precisions=(precision,),
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
        + tuple(f"layer-{index:02d}" for index in range(decoder_layers))
        + ("final-norm-head",)
    )
    edges = []
    for index in range(len(node_ids) - 1):
        producer = node_ids[index]
        consumer = node_ids[index + 1]
        if producer == "embedding":
            edge_id = "hidden-embedding-to-layer-00"
        elif consumer == "final-norm-head":
            edge_id = f"hidden-layer-{decoder_layers - 1}-to-final"
        else:
            left = int(producer.split("-")[1])
            right = int(consumer.split("-")[1])
            edge_id = f"hidden-layer-{left}-to-{right}"
        edges.append(TensorEdgeView(
            edge_id=edge_id,
            producer=producer,
            consumers=(consumer,),
            dtype=precision,
            shape=("batch", "sequence", "hidden"),
            estimated_bytes=None,
        ))
    graph_digest = canonical_contract_digest({
        "model": model_name,
        "revision": revision,
        "layer_ranges": ranges,
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
            "logits", precision, ("batch", "sequence", "vocabulary"), None),),
    )
    graph_port = StaticGraphAdapter(
        AdapterPortDescriptor(
            "qwen-three-stage-graph", "1", _digest("graph-port")),
        graph,
    )
    splitter_port = AdapterPortDescriptor(
        "qwen-three-stage-splitter", "1", _digest("splitter-port"))
    splitter = QwenThreeStageSplitter(
        splitter_port,
        SplitterDescriptor(
            splitter_port.name, splitter_port.version,
            splitter_port.state_digest),
        dict(artifact_digests_by_role),
        {role: int(value) for role, value in weight_bytes_by_role.items()},
        ranges,
    )
    task = BytesGenerationTaskAdapter(
        AdapterPortDescriptor(
            "qwen-generation-task", "1", _digest("task-port")),
        InferenceTaskDescriptor(
            "text-generation", input_schema, options_schema, result_schema),
        adapter_name,
    )
    state = StaticStateAdapter(
        AdapterPortDescriptor(
            "qwen-state", "1", _digest("state-port")),
        (
            _state_contract(
                "qwen", InferenceStateClass.REQUEST_SCOPED, retention_ms=0),
            _state_contract(
                "qwen", InferenceStateClass.EXACT_PREFIX_REUSABLE,
                retention_ms=300_000),
        ),
    )
    runner = GuardedRunnerAdapter(
        AdapterPortDescriptor(
            "qwen-runner", "1", _digest("runner-port")))
    return ModelFamilyAdapter(
        descriptor, graph_port, splitter, task, state, runner)


def build_qwen36_27b_three_stage_adapter(
    *,
    artifact_digests_by_role: Mapping[str, str],
    weight_bytes_by_role: Mapping[str, int],
) -> ModelFamilyAdapter:
    """Compatibility constructor for the pinned Qwen3.6-27B profile."""

    return build_qwen_three_stage_adapter(
        model_name=QWEN36_27B_MODEL,
        revision=QWEN36_27B_REVISION,
        layer_ranges=QWEN36_27B_LAYER_RANGES,
        artifact_digests_by_role=artifact_digests_by_role,
        weight_bytes_by_role=weight_bytes_by_role,
        precision="bfloat16",
        adapter_name="qwen36-27b-pipeline",
    )


__all__ = [
    "QWEN36_27B_LAYER_RANGES",
    "QWEN36_27B_MODEL",
    "QWEN36_27B_REVISION",
    "QWEN36_STAGE_ROLES",
    "QwenThreeStageSplitter",
    "build_qwen_three_stage_adapter",
    "build_qwen36_27b_three_stage_adapter",
]
