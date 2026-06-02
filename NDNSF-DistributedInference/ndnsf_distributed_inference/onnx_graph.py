"""Optional ONNX graph analysis helpers for NDNSF-DI split planning.

The core NDNSF-DI policy format remains model-agnostic: a splitter may be
handwritten, PyTorch-specific, ONNX-based, container-based, or supplied by an
external optimizer. This module is only an optional helper for ONNX models. It
extracts tensor/operator dependencies and can annotate chunk-level dependency
edges with the tensor names that cross each chunk boundary.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Sequence

from .plan import InferenceDependency


_ONNX_DTYPE_SIZES = {
    1: 4,   # FLOAT
    2: 1,   # UINT8
    3: 1,   # INT8
    4: 2,   # UINT16
    5: 2,   # INT16
    6: 4,   # INT32
    7: 8,   # INT64
    9: 1,   # BOOL
    10: 2,  # FLOAT16
    11: 8,  # DOUBLE
    12: 4,  # UINT32
    13: 8,  # UINT64
    16: 2,  # BFLOAT16
}

_ONNX_DTYPE_NAMES = {
    1: "float32",
    2: "uint8",
    3: "int8",
    4: "uint16",
    5: "int16",
    6: "int32",
    7: "int64",
    9: "bool",
    10: "float16",
    11: "float64",
    12: "uint32",
    13: "uint64",
    16: "bfloat16",
}


@dataclass(frozen=True)
class OnnxTensorInfo:
    name: str
    dtype: str = ""
    shape: tuple[int | str, ...] = ()
    size_bytes: int | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "dtype": self.dtype,
            "shape": list(self.shape),
            "sizeBytes": self.size_bytes,
        }


@dataclass(frozen=True)
class OnnxNodeInfo:
    index: int
    name: str
    op_type: str
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]

    def to_dict(self) -> dict[str, Any]:
        return {
            "index": self.index,
            "name": self.name,
            "opType": self.op_type,
            "inputs": list(self.inputs),
            "outputs": list(self.outputs),
        }


@dataclass(frozen=True)
class OnnxGraphSummary:
    model_path: str
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]
    initializers: tuple[str, ...]
    tensors: dict[str, OnnxTensorInfo]
    nodes: tuple[OnnxNodeInfo, ...]
    tensor_producers: dict[str, int]
    tensor_consumers: dict[str, tuple[int, ...]]

    def tensor(self, name: str) -> OnnxTensorInfo:
        return self.tensors.get(name, OnnxTensorInfo(name=name))

    def tensor_size(self, name: str) -> int | None:
        return self.tensor(name).size_bytes

    def to_dict(self) -> dict[str, Any]:
        return {
            "modelPath": self.model_path,
            "inputs": list(self.inputs),
            "outputs": list(self.outputs),
            "initializers": list(self.initializers),
            "tensors": {
                name: tensor.to_dict()
                for name, tensor in sorted(self.tensors.items())
            },
            "nodes": [node.to_dict() for node in self.nodes],
            "tensorProducers": dict(sorted(self.tensor_producers.items())),
            "tensorConsumers": {
                name: list(indices)
                for name, indices in sorted(self.tensor_consumers.items())
            },
        }


@dataclass(frozen=True)
class OnnxSplitCandidate:
    cut_after_node: int
    boundary_tensors: tuple[str, ...]
    known_boundary_bytes: int
    unknown_size_tensors: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {
            "cutAfterNode": self.cut_after_node,
            "boundaryTensors": list(self.boundary_tensors),
            "knownBoundaryBytes": self.known_boundary_bytes,
            "unknownSizeTensors": list(self.unknown_size_tensors),
        }


@dataclass(frozen=True)
class OnnxChunkSpec:
    role: str
    path: str
    key_scope: str = ""
    topic_prefix: str = "/activation"


@dataclass(frozen=True)
class OnnxChunkDependency:
    producer: str
    consumer: str
    key_scope: str
    topic_prefix: str
    tensors: tuple[str, ...]
    known_boundary_bytes: int
    unknown_size_tensors: tuple[str, ...] = ()

    def to_inference_dependency(self) -> InferenceDependency:
        return InferenceDependency(
            producers=[self.producer],
            consumers=[self.consumer],
            key_scope=self.key_scope,
            topic_prefix=self.topic_prefix,
            tensors=list(self.tensors),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "producer": self.producer,
            "consumer": self.consumer,
            "keyScope": self.key_scope,
            "topicPrefix": self.topic_prefix,
            "tensors": list(self.tensors),
            "knownBoundaryBytes": self.known_boundary_bytes,
            "unknownSizeTensors": list(self.unknown_size_tensors),
        }


def analyze_onnx_graph(path: str | Path) -> OnnxGraphSummary:
    """Analyze one ONNX graph and return tensor/operator dependency metadata."""

    try:
        import onnx  # type: ignore
    except ImportError as exc:
        raise RuntimeError("ONNX graph analysis requires the onnx Python package") from exc

    model_path = Path(path)
    model = onnx.load(str(model_path))
    try:
        model = onnx.shape_inference.infer_shapes(model)
    except Exception:
        pass

    graph = model.graph
    tensors: dict[str, OnnxTensorInfo] = {}
    for value in list(graph.input) + list(graph.output) + list(graph.value_info):
        info = _tensor_info_from_value_info(value)
        if info.name:
            tensors[info.name] = info
    for initializer in graph.initializer:
        tensors[initializer.name] = OnnxTensorInfo(
            name=initializer.name,
            dtype=_dtype_name(int(initializer.data_type)),
            shape=tuple(int(dim) for dim in initializer.dims),
            size_bytes=_shape_size_bytes(
                tuple(int(dim) for dim in initializer.dims),
                int(initializer.data_type),
            ),
        )

    nodes: list[OnnxNodeInfo] = []
    producers: dict[str, int] = {}
    consumers: dict[str, list[int]] = {}
    initializers = {initializer.name for initializer in graph.initializer}
    for index, node in enumerate(graph.node):
        inputs = tuple(name for name in node.input if name and name not in initializers)
        outputs = tuple(name for name in node.output if name)
        node_info = OnnxNodeInfo(
            index=index,
            name=node.name or f"{index}:{node.op_type}",
            op_type=node.op_type,
            inputs=inputs,
            outputs=outputs,
        )
        nodes.append(node_info)
        for output in outputs:
            producers[output] = index
        for input_name in inputs:
            consumers.setdefault(input_name, []).append(index)

    return OnnxGraphSummary(
        model_path=str(model_path),
        inputs=tuple(value.name for value in graph.input if value.name not in initializers),
        outputs=tuple(value.name for value in graph.output),
        initializers=tuple(sorted(initializers)),
        tensors=tensors,
        nodes=tuple(nodes),
        tensor_producers=producers,
        tensor_consumers={
            name: tuple(indices)
            for name, indices in consumers.items()
        },
    )


def estimate_split_candidates(
    summary: OnnxGraphSummary,
    *,
    max_candidates: int = 20,
) -> list[OnnxSplitCandidate]:
    """Estimate sequential split candidates from an ONNX tensor DAG.

    This does not decide placement. It ranks simple topological cuts by the
    known tensor bytes that must cross the cut. Unknown dynamic tensor sizes are
    kept explicit so a higher-level planner can reject or measure them.
    """

    candidates: list[OnnxSplitCandidate] = []
    if len(summary.nodes) < 2:
        return candidates

    graph_outputs = set(summary.outputs)
    for cut in range(len(summary.nodes) - 1):
        boundary: set[str] = set()
        for tensor, producer_index in summary.tensor_producers.items():
            if producer_index > cut:
                continue
            consumer_indices = summary.tensor_consumers.get(tensor, ())
            crosses = any(index > cut for index in consumer_indices)
            if crosses or tensor in graph_outputs:
                boundary.add(tensor)
        known_bytes = 0
        unknown: list[str] = []
        for tensor in sorted(boundary):
            size = summary.tensor_size(tensor)
            if size is None:
                unknown.append(tensor)
            else:
                known_bytes += size
        candidates.append(OnnxSplitCandidate(
            cut_after_node=cut,
            boundary_tensors=tuple(sorted(boundary)),
            known_boundary_bytes=known_bytes,
            unknown_size_tensors=tuple(unknown),
        ))

    if max_candidates <= 0 or len(candidates) <= max_candidates:
        return candidates

    by_transfer = sorted(candidates, key=_split_candidate_transfer_key)
    by_balance = sorted(
        candidates,
        key=lambda item: (
            abs((item.cut_after_node + 1) - (len(summary.nodes) / 2.0)),
            len(item.unknown_size_tensors),
            item.known_boundary_bytes,
            len(item.boundary_tensors),
        ),
    )
    by_combined = sorted(
        candidates,
        key=lambda item: (
            len(item.unknown_size_tensors),
            abs((item.cut_after_node + 1) - (len(summary.nodes) / 2.0)) /
            max(len(summary.nodes), 1),
            item.known_boundary_bytes,
            len(item.boundary_tensors),
        ),
    )

    selected: dict[int, OnnxSplitCandidate] = {}
    transfer_budget = max(1, max_candidates // 3)
    balance_budget = max(1, max_candidates // 3)
    combined_budget = max(1, max_candidates - transfer_budget - balance_budget)
    for source, budget in (
        (by_transfer, transfer_budget),
        (by_balance, balance_budget),
        (by_combined, combined_budget),
    ):
        for candidate in source:
            selected.setdefault(candidate.cut_after_node, candidate)
            if len(selected) >= max_candidates:
                break
            if sum(1 for item in source[:budget]
                   if item.cut_after_node in selected) >= budget:
                break

    for candidate in by_transfer:
        if len(selected) >= max_candidates:
            break
        selected.setdefault(candidate.cut_after_node, candidate)

    return sorted(selected.values(), key=_split_candidate_transfer_key)


def _split_candidate_transfer_key(candidate: OnnxSplitCandidate):
    return (
        len(candidate.unknown_size_tensors),
        candidate.known_boundary_bytes,
        len(candidate.boundary_tensors),
        candidate.cut_after_node,
    )


def build_sequential_chunk_dependencies(
    chunks: Sequence[OnnxChunkSpec],
) -> tuple[list[InferenceDependency], dict[str, Any]]:
    """Build chunk-level dependencies from exported ONNX chunk IO.

    Despite the historical name, this now inspects all chunk input/output
    tensors and builds a DAG of chunk-level collaboration dependencies. It
    handles branches, skip connections, fan-out, fan-in, concat-style inputs,
    and multi-output chunks as long as the exported ONNX chunks preserve tensor
    names at their boundaries.
    """

    return build_chunk_dependencies(chunks)


def build_chunk_dependencies(
    chunks: Sequence[OnnxChunkSpec],
) -> tuple[list[InferenceDependency], dict[str, Any]]:
    """Build a chunk collaboration graph from exported ONNX chunk IO.

    Each edge groups all tensors produced by one chunk and consumed by another
    chunk. The helper deliberately works at chunk granularity; the operator DAG
    remains inside each exported ONNX chunk.
    """

    summaries = {
        chunk.role: analyze_onnx_graph(chunk.path)
        for chunk in chunks
    }
    dependencies: list[InferenceDependency] = []
    chunk_edges: list[OnnxChunkDependency] = []
    for consumer_index, right in enumerate(chunks):
        consumer = summaries[right.role]
        grouped: dict[int, list[str]] = {}
        for input_name in consumer.inputs:
            producer_index, producer_tensor = _nearest_producer_for_input(
                input_name,
                chunks[:consumer_index],
                summaries,
            )
            if producer_index is None or producer_tensor is None:
                continue
            grouped.setdefault(producer_index, []).append(producer_tensor)

        for producer_index, tensors in sorted(grouped.items()):
            left = chunks[producer_index]
            edge = _make_chunk_dependency(
                left,
                right,
                summaries[left.role],
                _dedupe_preserving_order(tensors),
            )
            chunk_edges.append(edge)
            dependencies.append(edge.to_inference_dependency())

    if not chunk_edges:
        chunk_edges = _fallback_sequential_edges(chunks, summaries)
        dependencies = [edge.to_inference_dependency() for edge in chunk_edges]

    summary_dict = {
        "chunks": {
            role: summary.to_dict()
            for role, summary in summaries.items()
        },
        "dependencies": [edge.to_dict() for edge in chunk_edges],
        "roles": [
            {
                "role": chunk.role,
                "path": chunk.path,
                "keyScope": chunk.key_scope,
                "topicPrefix": chunk.topic_prefix,
            }
            for chunk in chunks
        ],
    }
    return dependencies, summary_dict


def _make_chunk_dependency(
    left: OnnxChunkSpec,
    right: OnnxChunkSpec,
    producer: OnnxGraphSummary,
    tensors: Sequence[str],
) -> OnnxChunkDependency:
    known_bytes = 0
    unknown: list[str] = []
    for tensor in tensors:
        size = producer.tensor_size(tensor)
        if size is None:
            unknown.append(tensor)
        else:
            known_bytes += size
    key_scope = left.key_scope or _default_key_scope(left.role, right.role)
    return OnnxChunkDependency(
        producer=left.role,
        consumer=right.role,
        key_scope=key_scope,
        topic_prefix=left.topic_prefix or "/activation",
        tensors=tuple(tensors),
        known_boundary_bytes=known_bytes,
        unknown_size_tensors=tuple(unknown),
    )


def _nearest_producer_for_input(
    input_name: str,
    upstream_chunks: Sequence[OnnxChunkSpec],
    summaries: dict[str, OnnxGraphSummary],
) -> tuple[int | None, str | None]:
    target = _canonical_boundary_name(input_name)
    for index in range(len(upstream_chunks) - 1, -1, -1):
        chunk = upstream_chunks[index]
        summary = summaries[chunk.role]
        for output in summary.outputs:
            if _canonical_boundary_name(output) == target:
                return index, output
    return None, None


def _canonical_boundary_name(name: str) -> str:
    base, dot, suffix = name.rpartition(".")
    if dot and suffix.isdigit():
        return base
    return name


def _dedupe_preserving_order(values: Sequence[str]) -> tuple[str, ...]:
    seen: set[str] = set()
    result: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return tuple(result)


def _fallback_sequential_edges(
    chunks: Sequence[OnnxChunkSpec],
    summaries: dict[str, OnnxGraphSummary],
) -> list[OnnxChunkDependency]:
    """Keep old adjacent behavior for chunk exports without stable tensor names."""

    edges: list[OnnxChunkDependency] = []
    for left, right in zip(chunks, chunks[1:]):
        producer = summaries[left.role]
        tensors = producer.outputs
        known_bytes = 0
        unknown: list[str] = []
        for tensor in tensors:
            size = producer.tensor_size(tensor)
            if size is None:
                unknown.append(tensor)
            else:
                known_bytes += size
        key_scope = left.key_scope or _default_key_scope(left.role, right.role)
        edges.append(OnnxChunkDependency(
            producer=left.role,
            consumer=right.role,
            key_scope=key_scope,
            topic_prefix=left.topic_prefix or "/activation",
            tensors=tensors,
            known_boundary_bytes=known_bytes,
            unknown_size_tensors=tuple(unknown),
        ))
    return edges


def write_onnx_graph_summary(
    path: str | Path,
    *,
    chunk_summary: dict[str, Any] | None = None,
    full_model_summary: OnnxGraphSummary | None = None,
    split_candidates: Sequence[OnnxSplitCandidate] = (),
    planner_recommendations: Sequence[Any] = (),
) -> None:
    payload: dict[str, Any] = {}
    if full_model_summary is not None:
        payload["fullModel"] = full_model_summary.to_dict()
    if split_candidates:
        payload["splitCandidates"] = [
            candidate.to_dict()
            for candidate in split_candidates
        ]
    if planner_recommendations:
        payload["plannerRecommendations"] = [
            recommendation.to_dict()
            for recommendation in planner_recommendations
        ]
    if chunk_summary is not None:
        payload["chunkGraph"] = chunk_summary
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def _default_key_scope(producer: str, consumer: str) -> str:
    return (
        producer.strip("/").replace("/", "-") +
        "-to-" +
        consumer.strip("/").replace("/", "-")
    )


def _tensor_info_from_value_info(value: Any) -> OnnxTensorInfo:
    tensor_type = value.type.tensor_type
    elem_type = int(tensor_type.elem_type)
    shape: list[int | str] = []
    static_shape = True
    for dim in tensor_type.shape.dim:
        if dim.HasField("dim_value"):
            shape.append(int(dim.dim_value))
        elif dim.HasField("dim_param"):
            shape.append(str(dim.dim_param))
            static_shape = False
        else:
            shape.append("?")
            static_shape = False
    return OnnxTensorInfo(
        name=value.name,
        dtype=_dtype_name(elem_type),
        shape=tuple(shape),
        size_bytes=_shape_size_bytes(tuple(shape), elem_type) if static_shape else None,
    )


def _shape_size_bytes(shape: tuple[int | str, ...], elem_type: int) -> int | None:
    elem_size = _ONNX_DTYPE_SIZES.get(elem_type)
    if elem_size is None:
        return None
    count = 1
    for dim in shape:
        if not isinstance(dim, int) or dim < 0:
            return None
        count *= dim
    return count * elem_size


def _dtype_name(elem_type: int) -> str:
    return _ONNX_DTYPE_NAMES.get(elem_type, f"onnx_type_{elem_type}")
