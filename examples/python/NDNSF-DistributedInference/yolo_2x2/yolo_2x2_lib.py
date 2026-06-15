"""Real YOLO layout distributed-inference example helpers.

The example uses a real Ultralytics YOLO nano model and exports ONNX chunks
according to a requested legacy chunk layout. The historical default is 2x2:
two pipeline stages represented by two sequential chunks per stage:

* Stage0/Shard0 runs the first quarter and publishes an internal activation.
* Stage0/Shard1 fetches it, continues Stage0, and publishes a stage boundary.
* Stage1/Shard0 fetches the stage-boundary activation and continues Stage1.
* Stage1/Shard1 fetches the Stage1 internal activation and returns predictions.

Other layouts such as 1x3, 2x3, 3x2, or 3x3 use the same role naming pattern
and dependency-driven executor. In this YOLO example, shards inside a stage are
sequential ONNX chunks, not tensor-parallel shards.

The module also contains experimental parallel YOLO splitters. The
parallel-output mode is a fan-in correctness scaffold that duplicates upstream
YOLO compute. The parallel Detect-scale mode is closer to the real model graph:
one shared backbone/neck chunk fans out feature maps to parallel Detect-head
scale shards and a merge chunk decodes final predictions. It is model-specific,
but it uses the same NDNSF-DI dependency executor and ONNX artifact policy.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import struct
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Sequence

import numpy as np
import onnxruntime as ort
from ndnsf import (
    LargeDataReference,
    encode_large_data_reference_payload,
    parse_large_data_reference_payload,
)

from ndnsf_distributed_inference import (
    InferenceDependency,
    OnnxChunkSpec,
    ProviderProfile,
    SequentialSplitCandidate,
    analyze_onnx_graph,
    build_sequential_chunk_dependencies,
    estimate_split_candidates,
    homogeneous_provider_profiles,
    nxm_stage_roles,
    recommend_sequential_splits,
    repo_manifest_from_large_data_reference,
    write_onnx_graph_summary,
)
from ndnsf_distributed_inference.plan import (
    ArtifactSpec,
    ModelFamily,
    ModelFormat,
    PlannerKind,
    RuntimeSpec,
)
from ndnsf_distributed_inference.planner_registry import (
    PlannerBackend,
    PlannerBackendRegistry,
    PlannerRequest,
    PlannerResult,
)
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)


SERVICE = "/AI/YOLO/2x2Inference"
GROUP = "/NDNSF-DistributeInference/example/group"
CONTROLLER = "/NDNSF-DistributeInference/example/controller"
USER = "/NDNSF-DistributeInference/example/user"
PROVIDER_PREFIX = "/NDNSF-DistributeInference/example/provider"
CONFIG_FILE = str(Path(__file__).with_name("yolo_policy.yaml"))
RUNTIME_NAME = "/Runtime/NDNSF/YOLO2x2/OnnxRuntime/v1"
REPO_SERVICE = "/NDNSF/DistributedRepo"
REPO_PROVIDER = PROVIDER_PREFIX + "/D"
COMPUTE_PROVIDER_IDS = ["", "A", "B", "C", "E", "F", "G", "H", "I"]

ROLE_S0_0 = "/Stage/0/Shard/0"
ROLE_S0_1 = "/Stage/0/Shard/1"
ROLE_S1_0 = "/Stage/1/Shard/0"
ROLE_S1_1 = "/Stage/1/Shard/1"
ROLE_MERGE = "/Merge"
ROLE_BACKBONE = "/Backbone"
ROLES = [ROLE_S0_0, ROLE_S0_1, ROLE_S1_0, ROLE_S1_1]
DEFAULT_LAYOUT = "2x2"
YOLO_LAYOUT_SEMANTICS = "pipeline-sequential-chunks"
YOLO_PARALLEL_OUTPUT_SEMANTICS = "parallel-output-channel-shards"
YOLO_PARALLEL_DETECT_SCALE_SEMANTICS = "parallel-detect-scale-shards"
YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS = (
    "parallel-detect-replicated-backbone-shards"
)

DEFAULT_MODEL = "yolo26n.pt"
DEFAULT_INPUT_SIZE = 32
BATCH_SIZE = 1


def _load_yolo_split_helpers():
    yolo_split_dir = Path(__file__).resolve().parents[1] / "yolo_split"
    if str(yolo_split_dir) not in sys.path:
        sys.path.insert(0, str(yolo_split_dir))
    from yolo_split_lib import (  # noqa: PLC0415
        first_tensor,
        full_forward as yolo_full_forward,
        load_yolo_model,
        split_index,
    )
    return first_tensor, yolo_full_forward, load_yolo_model, split_index


@contextmanager
def optional_local_nfd(enabled: bool) -> Iterator[None]:
    started_here = False
    if enabled:
        if shutil.which("nfd-start") is None or shutil.which("nfd-stop") is None:
            raise RuntimeError("nfd-start/nfd-stop are required for --start-local-nfd")
        running = subprocess.run(["pgrep", "-x", "nfd"],
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL,
                                 check=False).returncode == 0
        if not running:
            subprocess.run(["nfd-start"], check=True)
            started_here = True
    try:
        yield
    finally:
        if started_here:
            subprocess.run(["nfd-stop"], check=False,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)


def _npz_payload(values: dict) -> bytes:
    buffer = io.BytesIO()
    np.savez(buffer, **values)
    return buffer.getvalue()


def _load_npz_payload(payload: bytes) -> dict[str, np.ndarray]:
    with np.load(io.BytesIO(payload), allow_pickle=False) as npz:
        return {key: npz[key] for key in npz.files}


def _decode_native_tensor_bundle(payload: bytes) -> dict[str, np.ndarray]:
    magic = b"NDITB001"
    if not payload.startswith(magic):
        raise ValueError("payload is not an NDNSF-DI native tensor bundle")
    offset = len(magic)

    def read(fmt: str):
        nonlocal offset
        size = struct.calcsize(fmt)
        if offset + size > len(payload):
            raise ValueError("truncated NDNSF-DI native tensor bundle")
        value = struct.unpack_from(fmt, payload, offset)[0]
        offset += size
        return value

    tensors: dict[str, np.ndarray] = {}
    count = read("<I")
    for _ in range(count):
        name_size = read("<I")
        if offset + name_size > len(payload):
            raise ValueError("truncated NDNSF-DI native tensor name")
        name = payload[offset:offset + name_size].decode("utf-8")
        offset += name_size
        element_type = read("<I")
        rank = read("<I")
        shape = [read("<q") for _ in range(rank)]
        data_size = read("<Q")
        if offset + data_size > len(payload):
            raise ValueError("truncated NDNSF-DI native tensor payload")
        data = payload[offset:offset + data_size]
        offset += data_size
        if element_type != 1:
            raise ValueError(f"unsupported NDNSF-DI native tensor element type {element_type}")
        array = np.frombuffer(data, dtype="<f4").astype(np.float32, copy=True)
        if shape:
            array = array.reshape(tuple(shape))
        tensors[name] = array
    if offset != len(payload):
        raise ValueError("NDNSF-DI native tensor bundle has trailing bytes")
    return tensors


def encode_native_tensor_bundle(values: dict[str, np.ndarray]) -> bytes:
    payload = bytearray(b"NDITB001")
    payload += struct.pack("<I", len(values))
    for name, value in values.items():
        name_bytes = str(name).encode("utf-8")
        array = np.asarray(value, dtype=np.float32)
        payload += struct.pack("<I", len(name_bytes)) + name_bytes
        payload += struct.pack("<I", 1)
        payload += struct.pack("<I", array.ndim)
        for dim in array.shape:
            payload += struct.pack("<q", int(dim))
        data = array.astype("<f4", copy=False).tobytes()
        payload += struct.pack("<Q", len(data)) + data
    return bytes(payload)


def npz_payload(values: dict[str, np.ndarray]) -> bytes:
    buffer = io.BytesIO()
    np.savez(buffer, **values)
    return buffer.getvalue()


def load_npz_payload(payload: bytes) -> dict[str, np.ndarray]:
    with np.load(io.BytesIO(payload), allow_pickle=False) as npz:
        return {key: npz[key] for key in npz.files}


def select_tensor_payload(payload: bytes, tensors: list[str] | tuple[str, ...]) -> bytes:
    """Return an NPZ payload containing only tensors required by one edge."""

    requested = [str(tensor) for tensor in tensors if str(tensor)]
    if not requested:
        return payload
    values = load_npz_payload(payload)
    selected: dict[str, np.ndarray] = {}
    missing: list[str] = []
    for tensor in requested:
        try:
            selected[tensor] = _value_for_input(values, tensor)
        except KeyError:
            missing.append(tensor)
    if missing:
        raise KeyError(
            "activation payload missing dependency tensor(s): " +
            ", ".join(missing)
        )
    return npz_payload(selected)


def verify_tensor_payload(payload: bytes, tensors: list[str] | tuple[str, ...]) -> None:
    """Validate that a dependency payload contains every tensor the edge names."""

    if tensors:
        select_tensor_payload(payload, tensors)


def image_shape(input_size: int = DEFAULT_INPUT_SIZE) -> tuple[int, int, int, int]:
    return (BATCH_SIZE, 3, input_size, input_size)


def make_input(input_size: int = DEFAULT_INPUT_SIZE) -> np.ndarray:
    rng = np.random.default_rng(20260528)
    return rng.random(size=image_shape(input_size)).astype(np.float32)


def full_forward(model_name: str, image: np.ndarray) -> np.ndarray:
    _, yolo_full_forward, load_yolo_model, _ = _load_yolo_split_helpers()
    _, model = load_yolo_model(model_name)
    import torch
    x = torch.from_numpy(image.astype(np.float32))
    return yolo_full_forward(model, x).numpy().astype(np.float32)


def _canonical_detection_rows(value: np.ndarray) -> np.ndarray:
    rows = np.asarray(value, dtype=np.float32).reshape(-1, value.shape[-1])
    # YOLO postprocess can return the same detections in a different row order
    # after graph splitting. Sort by all columns so verification checks the
    # detection set rather than the incidental top-k ordering.
    keys = [rows[:, index] for index in range(rows.shape[1] - 1, -1, -1)]
    return rows[np.lexsort(keys)]


def compare_yolo_outputs(actual: np.ndarray,
                         expected: np.ndarray,
                         *,
                         atol: float = 1e-3,
                         rtol: float = 1e-4) -> tuple[bool, float, float]:
    actual = np.asarray(actual, dtype=np.float32)
    expected = np.asarray(expected, dtype=np.float32)
    if actual.shape != expected.shape:
        return False, float("inf"), float("inf")

    compare_actual = actual
    compare_expected = expected
    if actual.ndim >= 2 and actual.shape[-1] == 6:
        compare_actual = _canonical_detection_rows(actual)
        compare_expected = _canonical_detection_rows(expected)

    diff = np.abs(compare_actual - compare_expected)
    max_diff = float(diff.max()) if diff.size else 0.0
    mean_diff = float(diff.mean()) if diff.size else 0.0
    return bool(np.allclose(compare_actual, compare_expected,
                            atol=atol, rtol=rtol)), max_diff, mean_diff


def _module_input(module, x, saved):
    if module.f == -1:
        return x
    if isinstance(module.f, int):
        return saved[module.f]
    return [x if index == -1 else saved[index] for index in module.f]


def _run_chunk(model, start: int, end: int, x, saved):
    modules = model.model
    for i in range(start, end):
        module = modules[i]
        x = module(_module_input(module, x, saved))
        while len(saved) <= i:
            saved.append(None)
        saved[i] = x if module.i in model.save else None
    return x, saved


def _saved_indices(model, end: int) -> list[int]:
    return [int(index) for index in model.save if int(index) < end]


def normalize_layout(layout: str | None = None) -> str:
    text = (layout or DEFAULT_LAYOUT).strip().lower().replace("*", "x")
    match = re.fullmatch(r"(\d+)x(\d+)", text)
    if not match:
        raise ValueError(
            f"invalid YOLO split layout {layout!r}; expected ROWSxCOLS, "
            "for example 1x3, 2x3, 3x2, or 3x3"
        )
    stages = int(match.group(1))
    shards = int(match.group(2))
    if stages <= 0 or shards <= 0:
        raise ValueError(f"layout dimensions must be positive: {layout!r}")
    return f"{stages}x{shards}"


def parse_layout(layout: str | None = None) -> tuple[int, int]:
    normalized = normalize_layout(layout)
    left, right = normalized.split("x", 1)
    return int(left), int(right)


def layout_semantics_metadata(layout: str | None = None) -> dict:
    stages, shards = parse_layout(layout)
    return {
        "layout_notation": "ROWSxCOLS",
        "layout_rows": stages,
        "layout_cols": shards,
        "layout_stages": stages,
        "layout_shards_per_stage": shards,
        "layout_semantics": YOLO_LAYOUT_SEMANTICS,
        "stage_shards_parallel": False,
        "true_nxm_semantics": (
            "N stages with M parallel shards per stage; requires a tensor-/"
            "operator-sharded splitter, not this YOLO sequential chunk splitter"
        ),
    }


def parallel_output_layout_metadata(layout: str | None = None) -> dict:
    stages, shards = parse_layout(layout)
    return {
        "layout_notation": "NxM",
        "layout_rows": stages,
        "layout_cols": shards,
        "layout_stages": stages,
        "layout_shards_per_stage": shards,
        "layout_semantics": YOLO_PARALLEL_OUTPUT_SEMANTICS,
        "stage_shards_parallel": True,
        "merge_role": ROLE_MERGE,
        "shard_axis": 1,
        "duplicates_backbone_compute": True,
        "prototype_note": (
            "verifiable YOLO output-channel sharding prototype; stage shards "
            "run in parallel and merge prediction slices, but each Stage0 "
            "shard currently duplicates upstream YOLO backbone compute"
        ),
    }


def parallel_detect_scale_layout_metadata(layout: str | None = None) -> dict:
    stages, shards = parse_layout(layout)
    return {
        "layout_notation": "graph-backed NxM",
        "layout_rows": stages,
        "layout_cols": shards,
        "layout_stages": stages,
        "layout_shards_per_stage": shards,
        "layout_semantics": YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
        "stage_shards_parallel": True,
        "merge_role": ROLE_MERGE,
        "shared_backbone_role": ROLE_BACKBONE,
        "duplicates_backbone_compute": False,
        "prototype_note": (
            "YOLO-specific parallel detection-scale splitter: one shared "
            "backbone/neck chunk fans out feature maps to parallel Detect-head "
            "scale shards, then a merge chunk decodes the final predictions"
        ),
    }


def parallel_detect_replicated_backbone_layout_metadata(layout: str | None = None) -> dict:
    metadata = parallel_detect_scale_layout_metadata(layout)
    metadata.update({
        "layout_semantics": YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS,
        "shared_backbone_role": "",
        "duplicates_backbone_compute": True,
        "prototype_note": (
            "YOLO-specific parallel detection-scale splitter: each Detect-head "
            "shard carries its own backbone/neck copy and only candidate "
            "tensors cross nodes before the merge chunk"
        ),
    })
    return metadata


def planner_kind_for_layout_semantics(layout_semantics: str, split: dict | None = None) -> str:
    if split and split.get("planner_selected_candidate"):
        return PlannerKind.YOLO_DETECT_AUTO.value
    if layout_semantics == YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS:
        return PlannerKind.YOLO_DETECT_REPLICATED_BACKBONE.value
    if layout_semantics == YOLO_PARALLEL_DETECT_SCALE_SEMANTICS:
        return PlannerKind.YOLO_DETECT_SHARED_BACKBONE.value
    if layout_semantics == YOLO_PARALLEL_OUTPUT_SEMANTICS:
        return PlannerKind.YOLO_OUTPUT_CHANNEL_SHARDS.value
    return PlannerKind.YOLO_SEQUENTIAL_CHUNKS.value


def planner_descriptor_metadata(layout_semantics: str, split: dict | None = None) -> dict:
    planner_kind = planner_kind_for_layout_semantics(layout_semantics, split)
    planner = {
        "modelFamily": ModelFamily.YOLO_ONNX.value,
        "modelFormat": ModelFormat.ONNX.value,
        "plannerKind": planner_kind,
        "schemaVersion": 2,
        "layoutSemantics": layout_semantics,
    }
    if split:
        if split.get("planner_selected_candidate"):
            planner["selectedCandidate"] = split["planner_selected_candidate"]
        if split.get("planner_cost_summary"):
            planner["costSummary"] = split["planner_cost_summary"]
        if split.get("planner_compute_summary"):
            planner["computeSummary"] = split["planner_compute_summary"]
    return {
        "model_family": ModelFamily.YOLO_ONNX.value,
        "model_format": ModelFormat.ONNX.value,
        "planner_kind": planner_kind,
        "execution_plan_schema_version": 2,
        "planner": planner,
    }


def yolo_planner_registry() -> PlannerBackendRegistry:
    registry = PlannerBackendRegistry()
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.YOLO_SEQUENTIAL_CHUNKS,
        model_family=ModelFamily.YOLO_ONNX,
        model_format=ModelFormat.ONNX,
        name="YOLO sequential ONNX chunks",
        description="Historical YOLO ONNX module-list chunk planner.",
        metadata={
            "splitOptions": {},
        },
        handler=yolo_plan_from_request,
    ))
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.YOLO_OUTPUT_CHANNEL_SHARDS,
        model_family=ModelFamily.YOLO_ONNX,
        model_format=ModelFormat.ONNX,
        name="YOLO output-channel shard prototype",
        description="Parallel output-channel shard correctness prototype.",
        metadata={
            "splitOptions": {"parallel_output_shards": True},
        },
        handler=yolo_plan_from_request,
    ))
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.YOLO_DETECT_SHARED_BACKBONE,
        model_family=ModelFamily.YOLO_ONNX,
        model_format=ModelFormat.ONNX,
        name="YOLO Detect shared-backbone planner",
        description="Shared backbone/neck with parallel Detect scale heads.",
        metadata={
            "splitOptions": {"parallel_detect_scale_shards": True},
        },
        handler=yolo_plan_from_request,
    ))
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.YOLO_DETECT_REPLICATED_BACKBONE,
        model_family=ModelFamily.YOLO_ONNX,
        model_format=ModelFormat.ONNX,
        name="YOLO Detect replicated-backbone planner",
        description="Replicated backbone/neck with lower cross-node activation.",
        metadata={
            "splitOptions": {
                "parallel_detect_replicated_backbone_shards": True,
            },
        },
        handler=yolo_plan_from_request,
    ))
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.YOLO_DETECT_AUTO,
        model_family=ModelFamily.YOLO_ONNX,
        model_format=ModelFormat.ONNX,
        name="YOLO Detect auto planner",
        description="Scores shared and replicated Detect plans by compute and transfer cost.",
        metadata={
            "splitOptions": {"auto_parallel_detect_plan": True},
        },
        handler=yolo_plan_from_request,
    ))
    return registry


def yolo_planner_kind_from_options(*,
                                   parallel_output_shards: bool = False,
                                   parallel_detect_scale_shards: bool = False,
                                   parallel_detect_replicated_backbone_shards: bool = False,
                                   auto_parallel_detect_plan: bool = False) -> str:
    selected = [
        bool(parallel_output_shards),
        bool(parallel_detect_scale_shards),
        bool(parallel_detect_replicated_backbone_shards),
        bool(auto_parallel_detect_plan),
    ]
    if sum(selected) > 1:
        raise ValueError(
            "YOLO planner modes are mutually exclusive; select one planner kind")
    if auto_parallel_detect_plan:
        return PlannerKind.YOLO_DETECT_AUTO.value
    if parallel_detect_replicated_backbone_shards:
        return PlannerKind.YOLO_DETECT_REPLICATED_BACKBONE.value
    if parallel_detect_scale_shards:
        return PlannerKind.YOLO_DETECT_SHARED_BACKBONE.value
    if parallel_output_shards:
        return PlannerKind.YOLO_OUTPUT_CHANNEL_SHARDS.value
    return PlannerKind.YOLO_SEQUENTIAL_CHUNKS.value


def yolo_planner_split_options(planner_kind: str | PlannerKind) -> dict:
    backend = yolo_planner_registry().get(planner_kind)
    return dict((backend.metadata or {}).get("splitOptions", {}) or {})


def yolo_planner_request_from_options(
    *,
    output_dir: str | Path,
    model_name: str = DEFAULT_MODEL,
    input_size: int = DEFAULT_INPUT_SIZE,
    provider_profiles: list[ProviderProfile] | None = None,
    auto_split: bool = False,
    layout: str = DEFAULT_LAYOUT,
    parallel_output_shards: bool = False,
    parallel_detect_scale_shards: bool = False,
    parallel_detect_replicated_backbone_shards: bool = False,
    auto_parallel_detect_plan: bool = False,
) -> PlannerRequest:
    planner_kind = yolo_planner_kind_from_options(
        parallel_output_shards=parallel_output_shards,
        parallel_detect_scale_shards=parallel_detect_scale_shards,
        parallel_detect_replicated_backbone_shards=(
            parallel_detect_replicated_backbone_shards),
        auto_parallel_detect_plan=auto_parallel_detect_plan,
    )
    return PlannerRequest(
        planner_kind=planner_kind,
        model_family=ModelFamily.YOLO_ONNX,
        model_format=ModelFormat.ONNX,
        model_path=str(model_name),
        output_dir=str(output_dir),
        layout=normalize_layout(layout),
        input_size=int(input_size),
        provider_profiles=list(provider_profiles or []),
        options={
            "auto_split": bool(auto_split),
            **yolo_planner_split_options(planner_kind),
        },
        metadata={
            "layoutNotation": "ROWSxCOLS",
        },
    )


def yolo_plan_from_request(request: PlannerRequest) -> PlannerResult:
    split = _split_model_for_request(request)
    return PlannerResult(
        request=request,
        split_plan=split,
        score_summary=dict(split.get("planner_cost_summary") or {}),
        selected_candidate=dict(split.get("planner_selected_candidate") or {}),
        metadata={
            "layoutSemantics": str(split.get("layout_semantics", "")),
            "localVerifyRequired": True,
        },
    )


def planner_cost_summary(
    dependencies: Sequence[InferenceDependency],
    *,
    provider_profiles: Sequence[ProviderProfile] | None = None,
) -> dict:
    profiles = list(provider_profiles or default_planner_provider_profiles())
    bottleneck_mbps = min(
        [max(0.001, float(profile.uplink_mbps)) for profile in profiles] +
        [max(0.001, float(profile.downlink_mbps)) for profile in profiles],
    )
    representative_rtt_ms = max(
        [float(profile.rtt_ms) for profile in profiles] or [0.0],
    )
    edges = []
    total_bytes = 0
    total_segments = 0
    for dep in dependencies:
        expected_bytes = int(dep.expected_bytes or 0)
        expected_segments = int(dep.expected_segments or 0)
        total_bytes += expected_bytes
        total_segments += expected_segments
        transfer_ms = representative_rtt_ms + (
            expected_bytes * 8.0 / (bottleneck_mbps * 1000.0)
        )
        edges.append({
            "producers": list(dep.producers),
            "consumers": list(dep.consumers),
            "keyScope": dep.key_scope,
            "tensors": list(dep.tensors),
            "expectedBytes": expected_bytes,
            "expectedSegments": expected_segments,
            "estimatedTransferMs": transfer_ms,
        })
    return {
        "activationBytesTotal": total_bytes,
        "activationSegmentsTotal": total_segments,
        "edgeCount": len(edges),
        "estimatedTransferProfile": {
            "representativeRttMs": representative_rtt_ms,
            "bottleneckMbps": bottleneck_mbps,
            "note": (
                "coarse planner estimate from provider profile; measured "
                "MiniNDN latency also includes NFD, validation, scheduling, "
                "and retry effects"
            ),
        },
        "dominantEdge": max(edges, key=lambda item: item["expectedBytes"], default={}),
        "edges": edges,
    }


def _role_compute_summary(
    *,
    layout_semantics: str,
    role_compute_ms: dict[str, float],
    cost_summary: dict,
) -> dict:
    edges = cost_summary.get("edges", [])
    backbone_to_head_ms = [
        float(edge.get("estimatedTransferMs", 0.0))
        for edge in edges
        if str(edge.get("keyScope", "")).startswith("backbone-to-head")
    ]
    head_to_merge_ms = [
        float(edge.get("estimatedTransferMs", 0.0))
        for edge in edges
        if str(edge.get("keyScope", "")).startswith("detect-head-shard")
    ]
    head_compute_ms = [
        float(value)
        for role, value in role_compute_ms.items()
        if role.startswith("/Head/Shard/")
    ]
    if layout_semantics == YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS:
        critical_compute_ms = (
            max(head_compute_ms or [0.0]) +
            float(role_compute_ms.get(ROLE_MERGE, 0.0))
        )
    else:
        critical_compute_ms = (
            float(role_compute_ms.get(ROLE_BACKBONE, 0.0)) +
            max(head_compute_ms or [0.0]) +
            float(role_compute_ms.get(ROLE_MERGE, 0.0))
        )
    critical_transfer_ms = (
        max(backbone_to_head_ms or [0.0]) +
        max(head_to_merge_ms or [0.0])
    )
    return {
        "roleComputeMs": {
            role: float(value)
            for role, value in sorted(role_compute_ms.items())
        },
        "criticalComputeMs": critical_compute_ms,
        "criticalTransferMs": critical_transfer_ms,
        "estimatedTotalMs": critical_compute_ms + critical_transfer_ms,
        "computeNote": (
            "export-time PyTorch forward timing on one local host; use it as "
            "a relative planner signal, not as final runtime performance"
        ),
        "transferNote": (
            "critical transfer estimate uses planned activation edges, "
            "provider RTT, segment count, and bottleneck bandwidth"
        ),
    }


def _candidate_selection_score(split: dict, *, mode: str) -> dict:
    cost = split.get("planner_cost_summary") or {}
    compute = split.get("planner_compute_summary") or {}
    return {
        "mode": mode,
        "layoutSemantics": split.get("layout_semantics", ""),
        "selected": False,
        "estimatedTotalMs": float(compute.get("estimatedTotalMs", 0.0)),
        "criticalComputeMs": float(compute.get("criticalComputeMs", 0.0)),
        "criticalTransferMs": float(compute.get("criticalTransferMs", 0.0)),
        "activationBytesTotal": int(cost.get("activationBytesTotal", 0)),
        "activationSegmentsTotal": int(cost.get("activationSegmentsTotal", 0)),
        "edgeCount": int(cost.get("edgeCount", 0)),
        "providerRttMs": float(
            (cost.get("estimatedTransferProfile") or {}).get("representativeRttMs", 0.0)
        ),
        "bottleneckMbps": float(
            (cost.get("estimatedTransferProfile") or {}).get("bottleneckMbps", 0.0)
        ),
        "dominantEdge": (cost.get("dominantEdge") or {}).get("keyScope", ""),
    }


def split_auto_parallel_detect_model(output_dir: str | Path,
                                     *,
                                     model_name: str = DEFAULT_MODEL,
                                     input_size: int = DEFAULT_INPUT_SIZE,
                                     provider_profiles: list[ProviderProfile] | None = None,
                                     auto_split: bool = False,
                                     layout: str = DEFAULT_LAYOUT) -> dict:
    """Generate comparable YOLO Detect candidates and return the lowest score."""

    del auto_split  # Detect candidate boundaries are fixed for this model family.
    output = Path(output_dir)
    candidate_root = output / "planner-candidates"
    shared = split_parallel_detect_scale_model(
        candidate_root / "shared-backbone",
        model_name=model_name,
        input_size=input_size,
        provider_profiles=provider_profiles,
        layout=layout,
        replicate_backbone_shards=False,
    )
    replicated = split_parallel_detect_scale_model(
        candidate_root / "replicated-backbone",
        model_name=model_name,
        input_size=input_size,
        provider_profiles=provider_profiles,
        layout=layout,
        replicate_backbone_shards=True,
    )
    scores = [
        _candidate_selection_score(shared, mode="shared-backbone"),
        _candidate_selection_score(replicated, mode="replicated-backbone"),
    ]
    best_index, best = min(
        enumerate(scores),
        key=lambda item: (
            float(item[1].get("estimatedTotalMs", 0.0)),
            int(item[1].get("activationBytesTotal", 0)),
            int(item[1].get("activationSegmentsTotal", 0)),
        ),
    )
    scores[best_index]["selected"] = True
    selected = replicated if best["mode"] == "replicated-backbone" else shared
    selection = {
        "mode": best["mode"],
        "layout": normalize_layout(layout),
        "model": str(model_name),
        "inputSize": int(input_size),
        "candidates": scores,
        "selectionRule": (
            "minimize estimatedTotalMs; ties break by activation bytes and "
            "planned segment count"
        ),
    }
    selected["planner_candidate_scores"] = scores
    selected["planner_selected_candidate"] = selection
    output.mkdir(parents=True, exist_ok=True)
    (output / "planner-selection.json").write_text(
        json.dumps(selection, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    return selected


def roles_for_layout(layout: str | None = None) -> list[str]:
    stages, shards = parse_layout(layout)
    return [
        f"/Stage/{stage}/Shard/{shard}"
        for stage in range(stages)
        for shard in range(shards)
    ]


def parallel_output_roles_for_layout(layout: str | None = None) -> list[str]:
    stages, shards = parse_layout(layout)
    return nxm_stage_roles(stages, shards) + [ROLE_MERGE]


def parallel_detect_scale_roles_for_layout(layout: str | None = None) -> list[str]:
    _, shards = parse_layout(layout)
    return (
        [ROLE_BACKBONE] +
        [f"/Head/Shard/{shard}" for shard in range(shards)] +
        [ROLE_MERGE]
    )


def parallel_detect_replicated_backbone_roles_for_layout(layout: str | None = None) -> list[str]:
    _, shards = parse_layout(layout)
    return [f"/Head/Shard/{shard}" for shard in range(shards)] + [ROLE_MERGE]


def compute_provider_identities(count: int) -> list[str]:
    if count > len(COMPUTE_PROVIDER_IDS):
        raise ValueError(
            f"layout requires {count} compute providers but the example "
            f"defines only {len(COMPUTE_PROVIDER_IDS)} MiniNDN identities")
    identities = []
    for provider_id in COMPUTE_PROVIDER_IDS[:count]:
        if provider_id:
            identities.append(PROVIDER_PREFIX.rstrip("/") + "/" + provider_id)
        else:
            identities.append(PROVIDER_PREFIX)
    return identities


def service_name_for_layout(layout: str | None = None) -> str:
    normalized = normalize_layout(layout)
    if normalized == DEFAULT_LAYOUT:
        return SERVICE
    return f"/AI/YOLO/{normalized}Inference"


def yolo_inference_service(deployment) -> str:
    services = getattr(deployment, "services", None)
    if services is None and hasattr(deployment, "deployment"):
        services = getattr(deployment.deployment, "services", None)
    for service in services or ():
        name = getattr(service, "name", "")
        if name and str(name) != REPO_SERVICE:
            return str(name)
    return SERVICE


def layout_from_role_count(role_count: int) -> str:
    if int(role_count) == len(ROLES):
        return DEFAULT_LAYOUT
    return f"1x{int(role_count)}"


def is_first_role(role: str, roles: Sequence[str]) -> bool:
    return bool(roles) and str(role) == str(roles[0])


def is_final_role(role: str, roles: Sequence[str]) -> bool:
    return bool(roles) and str(role) == str(roles[-1])


def _even_module_boundaries(module_count: int, chunk_count: int) -> list[int]:
    if chunk_count < 1:
        raise ValueError("chunk_count must be positive")
    if chunk_count > module_count:
        raise ValueError(
            f"layout requires {chunk_count} chunks but YOLO model only has "
            f"{module_count} modules"
        )
    boundaries = [0]
    for index in range(1, chunk_count):
        raw = round(index * module_count / chunk_count)
        lower = boundaries[-1] + 1
        upper = module_count - (chunk_count - index)
        boundaries.append(max(lower, min(upper, raw)))
    boundaries.append(module_count)
    return boundaries


def _legacy_2x2_chunk_splits(model, split: int) -> dict[str, tuple[int, int, bool]]:
    module_count = len(model.model)
    stage0_mid = max(1, split // 2)
    stage1_mid = split + max(1, (module_count - split) // 2)
    return {
        ROLE_S0_0: (0, stage0_mid, False),
        ROLE_S0_1: (stage0_mid, split, False),
        ROLE_S1_0: (split, stage1_mid, False),
        ROLE_S1_1: (stage1_mid, module_count, True),
    }


def _chunk_splits(model, *, layout: str, split: int) -> dict[str, tuple[int, int, bool]]:
    normalized = normalize_layout(layout)
    if normalized == DEFAULT_LAYOUT:
        return _legacy_2x2_chunk_splits(model, split)
    roles = roles_for_layout(normalized)
    boundaries = _even_module_boundaries(len(model.model), len(roles))
    return {
        role: (boundaries[index], boundaries[index + 1], index == len(roles) - 1)
        for index, role in enumerate(roles)
    }


def _split_ranges(length: int, shards: int) -> list[tuple[int, int]]:
    if shards <= 0:
        raise ValueError("shards must be positive")
    ranges = []
    for shard in range(shards):
        start = round(shard * length / shards)
        end = round((shard + 1) * length / shards)
        ranges.append((start, end))
    return ranges


def _activation_name_template() -> str:
    return (
        "{producerProvider}/NDNSF/DI/ACTIVATION/"
        "{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}"
    )


def _tensor_nbytes(value) -> int:
    try:
        return int(value.detach().cpu().numpy().nbytes)
    except AttributeError:
        return int(np.asarray(value).nbytes)


def _canonical_onnx_io_name(name: str) -> str:
    base, dot, suffix = str(name).rpartition(".")
    if dot and suffix.isdigit():
        return base
    return str(name)


def _onnx_io_matches(path: Path,
                     input_names: Sequence[str],
                     output_names: Sequence[str]) -> bool:
    if not path.exists():
        return False
    try:
        session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    except Exception:
        return False
    actual_inputs = [item.name for item in session.get_inputs()]
    actual_outputs = [item.name for item in session.get_outputs()]
    return actual_inputs == list(input_names) and actual_outputs == list(output_names)


def _encoded_tensor_bundle_nbytes(payload: bytes,
                                  tensors: list[str] | tuple[str, ...]) -> int:
    return len(_npz_payload({
        "payload": np.frombuffer(select_tensor_payload(payload, tensors),
                                 dtype=np.uint8),
    }))


def _estimated_segments(byte_count: int) -> int:
    if byte_count <= 0:
        return 0
    # Collaboration publish_large_named uses a 7000-byte segment by default.
    # The model-specific splitter calibrates byte_count with an encoded tensor
    # bundle, so adding another envelope cushion here over-predicts segments
    # and leaves impossible extra Interests pending on boundary-sized tensors.
    payload_segment_size = 7000
    return max(1, (byte_count + payload_segment_size - 1) // payload_segment_size)


def split_model(output_dir: str | Path,
                model_name: str = DEFAULT_MODEL,
                input_size: int = DEFAULT_INPUT_SIZE,
                provider_profiles: list[ProviderProfile] | None = None,
                auto_split: bool = False,
                layout: str = DEFAULT_LAYOUT,
                parallel_output_shards: bool = False,
                parallel_detect_scale_shards: bool = False,
                parallel_detect_replicated_backbone_shards: bool = False,
                auto_parallel_detect_plan: bool = False) -> dict:
    request = yolo_planner_request_from_options(
        output_dir=output_dir,
        model_name=model_name,
        input_size=input_size,
        provider_profiles=provider_profiles,
        auto_split=auto_split,
        layout=layout,
        parallel_output_shards=parallel_output_shards,
        parallel_detect_scale_shards=parallel_detect_scale_shards,
        parallel_detect_replicated_backbone_shards=(
            parallel_detect_replicated_backbone_shards),
        auto_parallel_detect_plan=auto_parallel_detect_plan,
    )
    return yolo_planner_registry().plan(request).split_plan


def _split_model_for_request(request: PlannerRequest) -> dict:
    output_dir = request.output_dir
    model_name = request.model_path
    input_size = request.input_size or DEFAULT_INPUT_SIZE
    provider_profiles = list(request.provider_profiles or [])
    auto_split = bool(request.option("auto_split", False))
    layout = request.layout or DEFAULT_LAYOUT

    if request.option("auto_parallel_detect_plan", False):
        return split_auto_parallel_detect_model(
            output_dir,
            model_name=model_name,
            input_size=input_size,
            provider_profiles=provider_profiles,
            auto_split=auto_split,
            layout=layout,
        )
    if request.option("parallel_detect_scale_shards", False):
        return split_parallel_detect_scale_model(
            output_dir,
            model_name=model_name,
            input_size=input_size,
            provider_profiles=provider_profiles,
            auto_split=auto_split,
            layout=layout,
        )
    if request.option("parallel_detect_replicated_backbone_shards", False):
        return split_parallel_detect_scale_model(
            output_dir,
            model_name=model_name,
            input_size=input_size,
            provider_profiles=provider_profiles,
            auto_split=auto_split,
            layout=layout,
            replicate_backbone_shards=True,
        )
    if request.option("parallel_output_shards", False):
        return split_parallel_output_model(
            output_dir,
            model_name=model_name,
            input_size=input_size,
            provider_profiles=provider_profiles,
            auto_split=auto_split,
            layout=layout,
        )

    first_tensor, _, load_yolo_model, split_index = _load_yolo_split_helpers()
    import torch
    import torch.nn as nn

    class YoloChunk(nn.Module):
        def __init__(self, model, start: int, end: int,
                     input_saved: list[int], output_saved: list[int],
                     final: bool):
            super().__init__()
            self.model = model
            self.start = start
            self.end = end
            self.input_saved = input_saved
            self.output_saved = output_saved
            self.final = final

        def forward(self, x, *saved_values):
            saved = [None] * self.start
            for index, value in zip(self.input_saved, saved_values):
                saved[index] = value
            x, saved = _run_chunk(self.model, self.start, self.end, x, saved)
            x = first_tensor(x)
            if self.final:
                return x
            return tuple([x] + [saved[index] for index in self.output_saved])

    class YoloFull(nn.Module):
        def __init__(self, model):
            super().__init__()
            self.model = model

        def forward(self, x):
            return first_tensor(self.model(x))

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    layout = normalize_layout(layout)
    roles = roles_for_layout(layout)
    loaded_name, model = load_yolo_model(model_name)
    stem = Path(loaded_name).stem
    full_model_path = output / f"{stem}-full-{input_size}.onnx"
    paths = {}
    chunk_metadata = {}
    chunk_output_payloads: dict[str, bytes] = {}

    x = torch.from_numpy(make_input(input_size)).float()
    current_ort_values_by_name = {
        "images": x.detach().cpu().numpy().astype(np.float32),
    }
    if not _onnx_io_matches(full_model_path, ["images"], ["predictions"]):
        torch.onnx.export(
            YoloFull(model).eval(),
            x,
            str(full_model_path),
            input_names=["images"],
            output_names=["predictions"],
            opset_version=17,
            do_constant_folding=True,
        )

    full_summary = analyze_onnx_graph(full_model_path)
    split_candidates = estimate_split_candidates(full_summary, max_candidates=200)
    planner_recommendations = recommend_sequential_splits(
        [
            SequentialSplitCandidate.from_onnx_candidate(candidate)
            for candidate in split_candidates
        ],
        total_nodes=len(full_summary.nodes),
        providers=provider_profiles or default_planner_provider_profiles(),
        max_recommendations=10,
    )
    fallback_split = int(split_index(model))
    split_source = "yolo-fixed"
    planner_selected = {}
    if auto_split and planner_recommendations:
        selected = planner_recommendations[0]
        selected_node = full_summary.nodes[selected.candidate.cut_after_node].name
        split = _module_split_from_cut(
            selected_node,
            module_count=len(model.model),
            fallback=fallback_split,
        )
        split_source = "onnx-planner"
        planner_selected = {
            "planner_selected_cut_after_node": int(selected.candidate.cut_after_node),
            "planner_selected_node": selected_node,
            "planner_selected_score": float(selected.score),
            "planner_selected_transfer_ms": float(selected.transfer_ms),
            "planner_selected_compute_imbalance": float(selected.compute_imbalance),
        }
    else:
        split = fallback_split
    chunks = _chunk_splits(model, layout=layout, split=split)

    current_values = (x,)
    current_saved = []
    for role in roles:
        start, end, final = chunks[role]
        input_saved = list(current_saved)
        output_saved = [] if final else _saved_indices(model, end)
        chunk = YoloChunk(model, start, end, input_saved, output_saved, final).eval()
        path = output / f"{stem}-{role.strip('/').replace('/', '-')}-{input_size}.onnx"
        input_names = ["images" if start == 0 else "x"] + [
            f"saved_{index}" for index in input_saved
        ]
        output_names = ["predictions"] if final else [
            "x", *[f"saved_{index}" for index in output_saved]
        ]
        with torch.no_grad():
            outputs = chunk(*current_values)
            next_values = outputs if isinstance(outputs, tuple) else (outputs,)
        if not path.exists():
            torch.onnx.export(
                chunk,
                current_values,
                str(path),
                input_names=input_names,
                output_names=output_names,
                opset_version=17,
                do_constant_folding=True,
            )
        session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
        ort_input_names = [item.name for item in session.get_inputs()]
        ort_output_names = [item.name for item in session.get_outputs()]
        ort_inputs = {}
        canonical_values = {
            _canonical_onnx_io_name(name): value
            for name, value in current_ort_values_by_name.items()
        }
        for input_name in ort_input_names:
            if input_name in current_ort_values_by_name:
                ort_inputs[input_name] = current_ort_values_by_name[input_name]
                continue
            canonical = _canonical_onnx_io_name(input_name)
            if canonical in canonical_values:
                ort_inputs[input_name] = canonical_values[canonical]
                continue
            raise KeyError(f"missing ONNX calibration input {input_name}")
        ort_outputs = tuple(
            np.asarray(value, dtype=np.float32)
            for value in session.run(ort_output_names, ort_inputs)
        )
        ort_output_values = dict(zip(ort_output_names, ort_outputs))
        chunk_output_payloads[role] = npz_payload(ort_output_values)
        paths[role] = path
        chunk_metadata[role] = {
            "source_model": loaded_name,
            "input_size": input_size,
            "layout": layout,
            "split": split,
            "split_source": split_source,
            "start": start,
            "end": end,
            "input_saved_indices": input_saved,
            "output_saved_indices": output_saved,
            "final": final,
            **planner_selected,
        }
        current_values = next_values
        current_ort_values_by_name = ort_output_values
        current_saved = output_saved

    dependencies, chunk_graph = _build_yolo_onnx_dependencies(paths, roles=roles)
    dependencies = _calibrate_yolo_dependency_payload_sizes(
        dependencies,
        chunk_graph,
        chunk_output_payloads,
    )
    graph_summary = output / f"{stem}-{layout}-onnx-graph-summary.json"
    write_onnx_graph_summary(
        graph_summary,
        full_model_summary=full_summary,
        split_candidates=split_candidates,
        planner_recommendations=planner_recommendations,
        chunk_summary=chunk_graph,
    )

    return {
        "paths": paths,
        "full_model_path": full_model_path,
        "model": loaded_name,
        "input_size": input_size,
        "layout": layout,
        "service": service_name_for_layout(layout),
        "roles": roles,
        "split": split,
        "split_source": split_source,
        **planner_selected,
        "chunks": chunk_metadata,
        "dependencies": dependencies,
        "onnx_graph_summary": graph_summary,
        "onnx_split_candidates": split_candidates,
        "planner_recommendations": planner_recommendations,
    }


def split_parallel_output_model(output_dir: str | Path,
                                model_name: str = DEFAULT_MODEL,
                                input_size: int = DEFAULT_INPUT_SIZE,
                                provider_profiles: list[ProviderProfile] | None = None,
                                auto_split: bool = False,
                                layout: str = DEFAULT_LAYOUT) -> dict:
    """Export a verifiable true-NxM YOLO output-shard prototype.

    This splitter is intentionally conservative. Stage-0 shards run in
    parallel and each produces a slice of the final YOLO prediction tensor.
    Optional later stages are identity pass-through shards, and a merge role
    concatenates the slices. This proves the NDNSF-DI parallel stage/fan-in
    execution shape without pretending to be an efficient YOLO backbone split.
    """

    del auto_split  # The current prototype has no ONNX cut-point search step.
    first_tensor, _, load_yolo_model, _ = _load_yolo_split_helpers()
    import torch
    import torch.nn as nn

    class YoloFull(nn.Module):
        def __init__(self, model):
            super().__init__()
            self.model = model

        def forward(self, x):
            return first_tensor(self.model(x))

    class YoloOutputShard(nn.Module):
        def __init__(self, model, start: int, end: int):
            super().__init__()
            self.model = model
            self.start = start
            self.end = end

        def forward(self, x):
            predictions = first_tensor(self.model(x))
            return predictions[:, self.start:self.end, :]

    class IdentityShard(nn.Module):
        def forward(self, x):
            return x

    class MergeConcat(nn.Module):
        def forward(self, *values):
            if len(values) == 1:
                return values[0]
            return torch.cat(tuple(values), dim=1)

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    layout = normalize_layout(layout)
    stages, shards = parse_layout(layout)
    roles = parallel_output_roles_for_layout(layout)
    stage_roles = nxm_stage_roles(stages, shards)
    loaded_name, model = load_yolo_model(model_name)
    stem = Path(loaded_name).stem
    full_model_path = output / f"{stem}-full-{input_size}.onnx"
    paths: dict[str, Path] = {}
    chunk_metadata: dict[str, dict] = {}
    chunk_output_payloads: dict[str, bytes] = {}

    x = torch.from_numpy(make_input(input_size)).float()
    with torch.no_grad():
        expected = YoloFull(model).eval()(x)
    if expected.ndim < 2:
        raise ValueError("YOLO parallel-output splitter expects rank >= 2 predictions")
    ranges = _split_ranges(int(expected.shape[1]), shards)
    if any(end <= start for start, end in ranges):
        raise ValueError(
            f"prediction axis length {int(expected.shape[1])} cannot be split into "
            f"{shards} non-empty shards")

    if not full_model_path.exists():
        torch.onnx.export(
            YoloFull(model).eval(),
            x,
            str(full_model_path),
            input_names=["images"],
            output_names=["predictions"],
            opset_version=17,
            do_constant_folding=True,
        )

    full_summary = analyze_onnx_graph(full_model_path)
    split_candidates = estimate_split_candidates(full_summary, max_candidates=200)
    planner_recommendations = recommend_sequential_splits(
        [
            SequentialSplitCandidate.from_onnx_candidate(candidate)
            for candidate in split_candidates
        ],
        total_nodes=len(full_summary.nodes),
        providers=provider_profiles or default_planner_provider_profiles(),
        max_recommendations=10,
    )

    with torch.no_grad():
        stage_values = [
            expected[:, start:end, :].contiguous()
            for start, end in ranges
        ]

    for shard, (start, end) in enumerate(ranges):
        role = f"/Stage/0/Shard/{shard}"
        tensor_name = f"stage0_shard_{shard}"
        path = output / f"{stem}-{role.strip('/').replace('/', '-')}-{input_size}.onnx"
        if not path.exists():
            torch.onnx.export(
                YoloOutputShard(model, start, end).eval(),
                x,
                str(path),
                input_names=["images"],
                output_names=[tensor_name],
                opset_version=17,
                do_constant_folding=True,
            )
        paths[role] = path
        chunk_metadata[role] = {
            "source_model": loaded_name,
            "input_size": input_size,
            "layout": layout,
            "layout_semantics": YOLO_PARALLEL_OUTPUT_SEMANTICS,
            "stage": 0,
            "shard": shard,
            "output_tensor": tensor_name,
            "shard_axis": 1,
            "axis_start": start,
            "axis_end": end,
            "final": False,
            "duplicates_backbone_compute": True,
        }

    for stage in range(1, stages):
        next_values = []
        for shard, previous_value in enumerate(stage_values):
            role = f"/Stage/{stage}/Shard/{shard}"
            input_name = f"stage{stage - 1}_shard_{shard}"
            output_name = f"stage{stage}_shard_{shard}"
            path = output / f"{stem}-{role.strip('/').replace('/', '-')}-{input_size}.onnx"
            if not path.exists():
                torch.onnx.export(
                    IdentityShard().eval(),
                    previous_value,
                    str(path),
                    input_names=[input_name],
                    output_names=[output_name],
                    opset_version=17,
                    do_constant_folding=True,
                )
            paths[role] = path
            chunk_metadata[role] = {
                "source_model": loaded_name,
                "input_size": input_size,
                "layout": layout,
                "layout_semantics": YOLO_PARALLEL_OUTPUT_SEMANTICS,
                "stage": stage,
                "shard": shard,
                "input_tensor": input_name,
                "output_tensor": output_name,
                "shard_axis": 1,
                "axis_start": ranges[shard][0],
                "axis_end": ranges[shard][1],
                "final": False,
            }
            next_values.append(previous_value)
        stage_values = next_values

    merge_inputs = [f"stage{stages - 1}_shard_{shard}" for shard in range(shards)]
    merge_path = output / f"{stem}-Merge-{layout}-{input_size}.onnx"
    if not merge_path.exists():
        torch.onnx.export(
            MergeConcat().eval(),
            tuple(stage_values),
            str(merge_path),
            input_names=merge_inputs,
            output_names=["predictions"],
            opset_version=17,
            do_constant_folding=True,
        )
    paths[ROLE_MERGE] = merge_path
    chunk_metadata[ROLE_MERGE] = {
        "source_model": loaded_name,
        "input_size": input_size,
        "layout": layout,
        "layout_semantics": YOLO_PARALLEL_OUTPUT_SEMANTICS,
        "role_type": "merge",
        "input_tensors": merge_inputs,
        "output_tensor": "predictions",
        "shard_axis": 1,
        "final": True,
    }

    dependencies: list[InferenceDependency] = []
    chunk_graph = {
        "layout": layout,
        "layout_semantics": YOLO_PARALLEL_OUTPUT_SEMANTICS,
        "stage_shards_parallel": True,
        "dependencies": [],
    }
    for stage in range(stages - 1):
        for shard, value in enumerate(stage_values):
            tensor = f"stage{stage}_shard_{shard}"
            dep = InferenceDependency(
                producers=[f"/Stage/{stage}/Shard/{shard}"],
                consumers=[f"/Stage/{stage + 1}/Shard/{shard}"],
                key_scope=f"stage{stage}-to-stage{stage + 1}-shard{shard}",
                topic_prefix="/activation",
                tensors=[tensor],
                object_name_template=_activation_name_template(),
                expected_bytes=_tensor_nbytes(value),
                expected_segments=_estimated_segments(_tensor_nbytes(value)),
            )
            dependencies.append(dep)
            chunk_graph["dependencies"].append({
                "producers": dep.producers,
                "consumers": dep.consumers,
                "keyScope": dep.key_scope,
                "tensors": dep.tensors,
                "expectedBytes": dep.expected_bytes,
                "expectedSegments": dep.expected_segments,
            })
    merge_bytes = sum(_tensor_nbytes(value) for value in stage_values)
    merge_dep = InferenceDependency(
        producers=[f"/Stage/{stages - 1}/Shard/{shard}" for shard in range(shards)],
        consumers=[ROLE_MERGE],
        key_scope=f"stage{stages - 1}-to-merge",
        topic_prefix="/activation",
        tensors=merge_inputs,
        object_name_template=_activation_name_template(),
        expected_bytes=merge_bytes,
        expected_segments=_estimated_segments(merge_bytes),
    )
    dependencies.append(merge_dep)
    chunk_graph["dependencies"].append({
        "producers": merge_dep.producers,
        "consumers": merge_dep.consumers,
        "keyScope": merge_dep.key_scope,
        "tensors": merge_dep.tensors,
        "expectedBytes": merge_dep.expected_bytes,
        "expectedSegments": merge_dep.expected_segments,
    })

    graph_summary = output / f"{stem}-{layout}-parallel-output-onnx-graph-summary.json"
    write_onnx_graph_summary(
        graph_summary,
        full_model_summary=full_summary,
        split_candidates=split_candidates,
        planner_recommendations=planner_recommendations,
        chunk_summary=chunk_graph,
    )

    return {
        "paths": paths,
        "full_model_path": full_model_path,
        "model": loaded_name,
        "input_size": input_size,
        "layout": layout,
        "layout_semantics": YOLO_PARALLEL_OUTPUT_SEMANTICS,
        "service": service_name_for_layout(layout),
        "roles": roles,
        "stage_roles": stage_roles,
        "split": 0,
        "split_source": YOLO_PARALLEL_OUTPUT_SEMANTICS,
        "chunks": chunk_metadata,
        "dependencies": dependencies,
        "onnx_graph_summary": graph_summary,
        "onnx_split_candidates": split_candidates,
        "planner_recommendations": planner_recommendations,
    }


def split_parallel_detect_scale_model(output_dir: str | Path,
                                      model_name: str = DEFAULT_MODEL,
                                      input_size: int = DEFAULT_INPUT_SIZE,
                                      provider_profiles: list[ProviderProfile] | None = None,
                                      auto_split: bool = False,
                                      layout: str = DEFAULT_LAYOUT,
                                      replicate_backbone_shards: bool = False) -> dict:
    """Export a YOLO DAG splitter with shared backbone and parallel Detect shards."""

    del auto_split  # Detect-scale split points are fixed by the YOLO Detect head.
    first_tensor, _, load_yolo_model, _ = _load_yolo_split_helpers()
    import torch
    import torch.nn as nn

    class YoloFull(nn.Module):
        def __init__(self, model):
            super().__init__()
            self.model = model

        def forward(self, x):
            return first_tensor(self.model(x))

    class YoloBackboneFeatures(nn.Module):
        def __init__(self, model, feature_indices: Sequence[int]):
            super().__init__()
            self.model = model
            self.end = len(model.model) - 1
            self.feature_indices = [int(index) for index in feature_indices]

        def forward(self, x):
            saved = []
            _, saved = _run_chunk(self.model, 0, self.end, x, saved)
            return tuple(saved[index] for index in self.feature_indices)

    class YoloDetectHeadShard(nn.Module):
        def __init__(self,
                     detect,
                     scale_indices: Sequence[int],
                     anchor_offsets: dict[int, tuple[int, int]]):
            super().__init__()
            heads = detect.one2one if getattr(detect, "end2end", False) else detect.one2many
            self.box_head = heads["box_head"]
            self.cls_head = heads["cls_head"]
            self.scale_indices = [int(index) for index in scale_indices]
            self.anchor_offsets = {
                int(index): (int(start), int(end))
                for index, (start, end) in anchor_offsets.items()
            }
            self.reg_max = int(detect.reg_max)
            self.nc = int(detect.nc)
            self.max_det = int(detect.max_det)
            self.dfl = detect.dfl
            self.register_buffer("anchors_const", detect.anchors.detach().clone())
            self.register_buffer("strides_const", detect.strides.detach().clone())

        def forward(self, *features):
            box_values = []
            score_values = []
            anchors = []
            strides = []
            for scale, feature in zip(self.scale_indices, features):
                batch = feature.shape[0]
                boxes = self.box_head[scale](feature).view(batch, 4 * self.reg_max, -1)
                scores = self.cls_head[scale](feature).view(batch, self.nc, -1)
                box_values.append(boxes)
                score_values.append(scores)
                start, end = self.anchor_offsets[scale]
                anchors.append(self.anchors_const[:, start:end])
                strides.append(self.strides_const[:, start:end])
            boxes = torch.cat(tuple(box_values), dim=-1)
            scores = torch.cat(tuple(score_values), dim=-1)
            anchor_values = torch.cat(tuple(anchors), dim=-1)
            stride_values = torch.cat(tuple(strides), dim=-1)
            dbox = detect.decode_bboxes(
                self.dfl(boxes),
                anchor_values.unsqueeze(0),
            ) * stride_values
            candidates = torch.cat((dbox, scores.sigmoid()), 1).permute(0, 2, 1)
            max_score = candidates[..., 4:].max(dim=-1)[0]
            k = self.max_det if candidates.shape[1] >= self.max_det else candidates.shape[1]
            _, index = max_score.topk(k, dim=1)
            return candidates.gather(
                1,
                index.unsqueeze(-1).repeat(1, 1, 4 + self.nc),
            )

    class YoloDetectReplicatedBackboneShard(nn.Module):
        def __init__(self,
                     model,
                     detect,
                     feature_indices: Sequence[int],
                     scale_indices: Sequence[int],
                     anchor_offsets: dict[int, tuple[int, int]]):
            super().__init__()
            self.backbone = YoloBackboneFeatures(model, feature_indices)
            self.head = YoloDetectHeadShard(detect, scale_indices, anchor_offsets)
            self.scale_indices = [int(index) for index in scale_indices]

        def forward(self, x):
            features = self.backbone(x)
            return self.head(*(features[index] for index in self.scale_indices))

    class YoloDetectMerge(nn.Module):
        def __init__(self, detect):
            super().__init__()
            self.max_det = int(detect.max_det)
            self.nc = int(detect.nc)
            self.agnostic_nms = bool(getattr(detect, "agnostic_nms", False))

        def forward(self, *values):
            candidates = torch.cat(tuple(values), dim=1)
            boxes, scores = candidates.split([4, self.nc], dim=-1)
            k = self.max_det if candidates.shape[1] >= self.max_det else candidates.shape[1]
            if self.agnostic_nms:
                scores, labels = scores.max(dim=-1, keepdim=True)
                scores, indices = scores.topk(k, dim=1)
                labels = labels.gather(1, indices)
                boxes = boxes.gather(dim=1, index=indices.repeat(1, 1, 4))
                return torch.cat([boxes, scores, labels.float()], dim=-1)
            anchor_index = scores.max(dim=-1)[0].topk(k)[1].unsqueeze(-1)
            selected_scores = scores.gather(dim=1,
                                            index=anchor_index.repeat(1, 1, self.nc))
            selected_scores, class_index = selected_scores.flatten(1).topk(k)
            box_index = anchor_index[
                torch.arange(candidates.shape[0], device=candidates.device)[..., None],
                class_index // self.nc,
            ]
            selected_boxes = boxes.gather(dim=1, index=box_index.repeat(1, 1, 4))
            return torch.cat([
                selected_boxes,
                selected_scores[..., None],
                (class_index % self.nc)[..., None].float(),
            ], dim=-1)

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    layout = normalize_layout(layout)
    _, shards = parse_layout(layout)
    loaded_name, model = load_yolo_model(model_name)
    detect = model.model[-1]
    feature_indices = [int(index) for index in detect.f]
    if shards > len(feature_indices):
        raise ValueError(
            f"YOLO Detect has {len(feature_indices)} scale features; "
            f"parallel-detect-scale-shards cannot create {shards} non-empty shards")
    scale_ranges = _split_ranges(len(feature_indices), shards)
    scale_groups = [
        list(range(start, end))
        for start, end in scale_ranges
        if end > start
    ]
    head_roles = [f"/Head/Shard/{index}" for index in range(len(scale_groups))]
    roles = ([*head_roles, ROLE_MERGE] if replicate_backbone_shards
             else [ROLE_BACKBONE, *head_roles, ROLE_MERGE])
    layout_semantics = (
        YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS
        if replicate_backbone_shards else YOLO_PARALLEL_DETECT_SCALE_SEMANTICS
    )
    stem = Path(loaded_name).stem
    full_model_path = output / f"{stem}-full-{input_size}.onnx"
    paths: dict[str, Path] = {}
    chunk_metadata: dict[str, dict] = {}
    chunk_output_payloads: dict[str, bytes] = {}
    role_compute_ms: dict[str, float] = {}

    x = torch.from_numpy(make_input(input_size)).float()
    with torch.no_grad():
        expected = YoloFull(model).eval()(x)
        started = time.perf_counter()
        feature_values = YoloBackboneFeatures(model, feature_indices).eval()(x)
        role_compute_ms[ROLE_BACKBONE] = (time.perf_counter() - started) * 1000.0
    anchor_offsets: dict[int, tuple[int, int]] = {}
    cursor = 0
    for index, value in enumerate(feature_values):
        anchors = int(value.shape[-2] * value.shape[-1])
        anchor_offsets[index] = (cursor, cursor + anchors)
        cursor += anchors

    if not full_model_path.exists():
        torch.onnx.export(
            YoloFull(model).eval(),
            x,
            str(full_model_path),
            input_names=["images"],
            output_names=["predictions"],
            opset_version=17,
            do_constant_folding=True,
        )

    feature_tensor_names = [
        f"detect_feature_{index}"
        for index in range(len(feature_indices))
    ]
    if not replicate_backbone_shards:
        backbone_path = output / f"{stem}-Backbone-{input_size}.onnx"
        if not _onnx_io_matches(backbone_path, ["images"], feature_tensor_names):
            torch.onnx.export(
                YoloBackboneFeatures(model, feature_indices).eval(),
                x,
                str(backbone_path),
                input_names=["images"],
                output_names=feature_tensor_names,
                opset_version=17,
                do_constant_folding=True,
            )
        paths[ROLE_BACKBONE] = backbone_path
        chunk_output_payloads[ROLE_BACKBONE] = npz_payload({
            name: np.asarray(value.detach().cpu().numpy(), dtype=np.float32)
            for name, value in zip(feature_tensor_names, feature_values)
        })
        chunk_metadata[ROLE_BACKBONE] = {
            "source_model": loaded_name,
            "input_size": input_size,
            "layout": layout,
            "layout_semantics": layout_semantics,
            "role_type": "shared-backbone-neck",
            "feature_indices": feature_indices,
            "output_tensors": feature_tensor_names,
            "final": False,
        }

    head_outputs: dict[str, torch.Tensor] = {}
    for shard, group in enumerate(scale_groups):
        role = f"/Head/Shard/{shard}"
        input_names = (["images"] if replicate_backbone_shards
                       else [feature_tensor_names[index] for index in group])
        output_names = [f"candidates_shard_{shard}"]
        input_values = (x,) if replicate_backbone_shards else tuple(
            feature_values[index] for index in group)
        with torch.no_grad():
            if replicate_backbone_shards:
                shard_model = YoloDetectReplicatedBackboneShard(
                    model,
                    detect,
                    feature_indices,
                    group,
                    anchor_offsets,
                ).eval()
            else:
                shard_model = YoloDetectHeadShard(detect, group, anchor_offsets).eval()
            started = time.perf_counter()
            outputs = (shard_model(*input_values),)
            role_compute_ms[role] = (time.perf_counter() - started) * 1000.0
        for name, value in zip(output_names, outputs):
            head_outputs[name] = value
        chunk_output_payloads[role] = npz_payload({
            name: np.asarray(value.detach().cpu().numpy(), dtype=np.float32)
            for name, value in zip(output_names, outputs)
        })
        path = output / f"{stem}-{role.strip('/').replace('/', '-')}-{input_size}.onnx"
        if not _onnx_io_matches(path, input_names, output_names):
            torch.onnx.export(
                shard_model,
                input_values,
                str(path),
                input_names=input_names,
                output_names=output_names,
                opset_version=17,
                do_constant_folding=True,
            )
        paths[role] = path
        chunk_metadata[role] = {
            "source_model": loaded_name,
            "input_size": input_size,
            "layout": layout,
            "layout_semantics": layout_semantics,
            "role_type": ("detect-head-replicated-backbone-candidate-filter"
                          if replicate_backbone_shards
                          else "detect-head-scale-shard-candidate-filter"),
            "shard": shard,
            "scale_indices": group,
            "input_tensors": input_names,
            "output_tensors": output_names,
            "duplicates_backbone_compute": bool(replicate_backbone_shards),
            "final": False,
        }

    merge_inputs = [f"candidates_shard_{shard}" for shard in range(len(scale_groups))]
    merge_values = tuple(head_outputs[name] for name in merge_inputs)
    merge_path = output / f"{stem}-DetectMerge-{layout}-{input_size}.onnx"
    if not _onnx_io_matches(merge_path, merge_inputs, ["predictions"]):
        torch.onnx.export(
            YoloDetectMerge(detect).eval(),
            merge_values,
            str(merge_path),
            input_names=merge_inputs,
            output_names=["predictions"],
            opset_version=17,
            do_constant_folding=True,
        )
    paths[ROLE_MERGE] = merge_path
    with torch.no_grad():
        started = time.perf_counter()
        _ = YoloDetectMerge(detect).eval()(*merge_values)
        role_compute_ms[ROLE_MERGE] = (time.perf_counter() - started) * 1000.0
    chunk_metadata[ROLE_MERGE] = {
        "source_model": loaded_name,
        "input_size": input_size,
        "layout": layout,
        "layout_semantics": layout_semantics,
        "role_type": "detect-merge-global-topk",
        "input_tensors": merge_inputs,
        "output_tensor": "predictions",
        "final": True,
    }

    full_summary = analyze_onnx_graph(full_model_path)
    split_candidates = estimate_split_candidates(full_summary, max_candidates=200)
    planner_recommendations = recommend_sequential_splits(
        [
            SequentialSplitCandidate.from_onnx_candidate(candidate)
            for candidate in split_candidates
        ],
        total_nodes=len(full_summary.nodes),
        providers=provider_profiles or default_planner_provider_profiles(),
        max_recommendations=10,
    )

    dependencies: list[InferenceDependency] = []
    chunk_graph = {
        "layout": layout,
        "layout_semantics": layout_semantics,
        "stage_shards_parallel": True,
        "duplicates_backbone_compute": bool(replicate_backbone_shards),
        "dependencies": [],
    }
    if not replicate_backbone_shards:
        for shard, group in enumerate(scale_groups):
            tensors = [feature_tensor_names[index] for index in group]
            expected_bytes = sum(_tensor_nbytes(feature_values[index]) for index in group)
            dep = InferenceDependency(
                producers=[ROLE_BACKBONE],
                consumers=[f"/Head/Shard/{shard}"],
                key_scope=f"backbone-to-head-shard{shard}",
                topic_prefix="/activation",
                tensors=tensors,
                object_name_template=_activation_name_template(),
                expected_bytes=expected_bytes,
                expected_segments=_estimated_segments(expected_bytes),
            )
            dependencies.append(dep)
            chunk_graph["dependencies"].append({
                "producers": dep.producers,
                "consumers": dep.consumers,
                "keyScope": dep.key_scope,
                "tensors": dep.tensors,
                "expectedBytes": dep.expected_bytes,
                "expectedSegments": dep.expected_segments,
            })

    for shard, group in enumerate(scale_groups):
        tensors = [f"candidates_shard_{shard}"]
        expected_bytes = sum(_tensor_nbytes(head_outputs[name]) for name in tensors)
        dep = InferenceDependency(
            producers=[f"/Head/Shard/{shard}"],
            consumers=[ROLE_MERGE],
            key_scope=f"detect-head-shard{shard}-to-merge",
            topic_prefix="/activation",
            tensors=tensors,
            object_name_template=_activation_name_template(),
            expected_bytes=expected_bytes,
            expected_segments=_estimated_segments(expected_bytes),
        )
        dependencies.append(dep)
        chunk_graph["dependencies"].append({
            "producers": dep.producers,
            "consumers": dep.consumers,
            "keyScope": dep.key_scope,
            "tensors": dep.tensors,
            "expectedBytes": dep.expected_bytes,
            "expectedSegments": dep.expected_segments,
        })

    dependencies = _calibrate_yolo_dependency_payload_sizes(
        dependencies,
        chunk_graph,
        chunk_output_payloads,
    )
    cost_summary = planner_cost_summary(
        dependencies,
        provider_profiles=provider_profiles,
    )
    compute_summary = _role_compute_summary(
        layout_semantics=layout_semantics,
        role_compute_ms=role_compute_ms,
        cost_summary=cost_summary,
    )
    chunk_graph["plannerCostSummary"] = cost_summary
    chunk_graph["plannerComputeSummary"] = compute_summary

    graph_summary = output / f"{stem}-{layout}-parallel-detect-scale-onnx-graph-summary.json"
    write_onnx_graph_summary(
        graph_summary,
        full_model_summary=full_summary,
        split_candidates=split_candidates,
        planner_recommendations=planner_recommendations,
        chunk_summary=chunk_graph,
    )

    return {
        "paths": paths,
        "full_model_path": full_model_path,
        "model": loaded_name,
        "input_size": input_size,
        "layout": layout,
        "layout_semantics": layout_semantics,
        "service": service_name_for_layout(layout),
        "roles": roles,
        "stage_roles": head_roles,
        "split": 0,
        "split_source": layout_semantics,
        "chunks": chunk_metadata,
        "dependencies": dependencies,
        "planner_cost_summary": cost_summary,
        "planner_compute_summary": compute_summary,
        "onnx_graph_summary": graph_summary,
        "onnx_split_candidates": split_candidates,
        "planner_recommendations": planner_recommendations,
        "reference_output_shape": list(expected.shape),
    }


def default_planner_provider_profiles() -> list[ProviderProfile]:
    return homogeneous_provider_profiles([
        "/NDNSF-DistributeInference/example/provider/A",
        "/NDNSF-DistributeInference/example/provider/B",
    ], uplink_mbps=1000.0, downlink_mbps=1000.0, rtt_ms=4.0)


def load_provider_profiles(path: str | Path) -> list[ProviderProfile]:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if isinstance(data, dict):
        data = data.get("providers", [])
    if not isinstance(data, list):
        raise ValueError("provider profile file must contain a list or {providers: [...]}")
    return [ProviderProfile.from_dict(item) for item in data]


def _module_split_from_cut(node_name: str, *, module_count: int, fallback: int) -> int:
    match = re.search(r"/model(?:/model)?\.(\d+)(?:/|$)", node_name)
    if not match:
        return fallback
    module_index = int(match.group(1))
    return max(2, min(module_count - 2, module_index + 1))


def _role_indices(role: str) -> tuple[int, int]:
    match = re.fullmatch(r"/?Stage/(\d+)/Shard/(\d+)/?", role)
    if not match:
        return 0, 0
    return int(match.group(1)), int(match.group(2))


def _edge_scope_for_role(role: str, roles: Sequence[str]) -> str:
    try:
        index = list(roles).index(role)
    except ValueError:
        return role.strip("/").replace("/", "-")
    if index >= len(roles) - 1:
        return ""
    stage, shard = _role_indices(role)
    next_stage, next_shard = _role_indices(roles[index + 1])
    if stage == next_stage:
        return f"stage{stage}-internal-{shard}-to-{next_shard}"
    return f"stage{stage}-to-stage{next_stage}"


def _build_yolo_onnx_dependencies(paths: dict[str, Path],
                                  *,
                                  roles: Sequence[str]) -> tuple[list[InferenceDependency], dict]:
    return build_sequential_chunk_dependencies([
        OnnxChunkSpec(
            role=role,
            path=str(paths[role]),
            key_scope=_edge_scope_for_role(role, roles),
        )
        for role in roles
    ])


def _calibrate_yolo_dependency_payload_sizes(
    dependencies: list[InferenceDependency],
    chunk_graph: dict,
    chunk_output_payloads: dict[str, bytes],
) -> list[InferenceDependency]:
    """Use the sample YOLO chunk run to estimate actual encoded bundle size.

    The generic ONNX graph analyzer only sees tensor shapes, so its
    ``expected_bytes`` is raw tensor payload. The runtime publishes selected
    tensors as an NPZ payload and then wraps that payload in a tensor-bundle NPZ.
    For this model-specific splitter we can do better because the chunk export
    path already ran one deterministic sample forward pass.
    """

    calibrated: list[InferenceDependency] = []
    encoded_bytes_by_edge: dict[tuple[str, str, str], int] = {}
    for dep in dependencies:
        producer = dep.producers[0] if dep.producers else ""
        consumer = dep.consumers[0] if dep.consumers else ""
        payload = chunk_output_payloads.get(producer)
        encoded_bytes = 0
        if payload is not None:
            encoded_bytes = _encoded_tensor_bundle_nbytes(payload, dep.tensors)
        if encoded_bytes <= 0:
            encoded_bytes = dep.expected_bytes
        calibrated_dep = InferenceDependency(
            producers=list(dep.producers),
            consumers=list(dep.consumers),
            key_scope=dep.key_scope,
            topic_prefix=dep.topic_prefix,
            required=dep.required,
            tensors=list(dep.tensors),
            object_name_template=dep.object_name_template,
            expected_segments=_estimated_segments(encoded_bytes),
            expected_bytes=encoded_bytes,
        )
        calibrated.append(calibrated_dep)
        encoded_bytes_by_edge[(producer, consumer, dep.key_scope)] = encoded_bytes

    for item in chunk_graph.get("dependencies", []):
        producer = str(item.get("producer", item.get("producers", [""])[0]
                       if item.get("producers") else ""))
        consumer = str(item.get("consumer", item.get("consumers", [""])[0]
                       if item.get("consumers") else ""))
        key_scope = str(item.get("keyScope", item.get("key_scope", "")))
        encoded_bytes = encoded_bytes_by_edge.get((producer, consumer, key_scope), 0)
        if encoded_bytes <= 0:
            continue
        item["encodedBundleBytes"] = encoded_bytes
        item["expectedBytes"] = encoded_bytes
        item["expectedSegments"] = _estimated_segments(encoded_bytes)
    return calibrated


def _manual_yolo_dependencies() -> list[InferenceDependency]:
    return [
        InferenceDependency(
            producers=[ROLE_S0_0],
            consumers=[ROLE_S0_1],
            key_scope="stage0-internal",
            topic_prefix="/activation",
        ),
        InferenceDependency(
            producers=[ROLE_S0_1],
            consumers=[ROLE_S1_0],
            key_scope="stage0-to-stage1",
            topic_prefix="/activation",
        ),
        InferenceDependency(
            producers=[ROLE_S1_0],
            consumers=[ROLE_S1_1],
            key_scope="stage1-internal",
            topic_prefix="/activation",
        ),
    ]


def yolo_splitter_output(split: dict) -> SplitterOutput:
    layout = normalize_layout(str(split.get("layout", DEFAULT_LAYOUT)))
    service_name = str(split.get("service", service_name_for_layout(layout)))
    roles = list(split.get("roles") or roles_for_layout(layout))
    artifact_prefix = f"/Model/Ultralytics/YOLO/{layout}"
    layout_semantics = str(split.get("layout_semantics", YOLO_LAYOUT_SEMANTICS))
    if layout_semantics == YOLO_PARALLEL_DETECT_SCALE_SEMANTICS:
        layout_metadata = parallel_detect_scale_layout_metadata(layout)
    elif layout_semantics == YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS:
        layout_metadata = parallel_detect_replicated_backbone_layout_metadata(layout)
    elif layout_semantics == YOLO_PARALLEL_OUTPUT_SEMANTICS:
        layout_metadata = parallel_output_layout_metadata(layout)
    else:
        layout_metadata = layout_semantics_metadata(layout)
    artifacts = []
    for role, path in split["paths"].items():
        artifacts.append(SplitArtifact(
            role=role,
            path=str(path),
            artifact_name=artifact_prefix + role,
            kind="onnx-model",
            backend="onnxruntime",
            metadata=dict(split["chunks"][role], shard_role=role),
        ))
    service = SplitServiceSpec(
        name=service_name,
        model_name=artifact_prefix,
        roles=roles,
        dependencies=list(split.get("dependencies") or _manual_yolo_dependencies()),
        artifacts=artifacts,
        input_schema={
            "codec": "npz",
            "encoder": "encode_image_for_yolo(image_tensor)",
            "fields": {
                "images": {
                    "dtype": "float32",
                    "shape": list(image_shape(int(split["input_size"]))),
                    "layout": "NCHW",
                },
            },
        },
        output_schema={
            "codec": "npz",
            "decoder": "decode_yolo_output(response.payload)",
            "fields": {
                "output": {
                    "dtype": "float32",
                },
            },
        },
        metadata={
            "source_model": str(split["model"]),
            "input_size": int(split["input_size"]),
            "layout": layout,
            **planner_descriptor_metadata(layout_semantics, split),
            **layout_metadata,
            "chunk_count": len(roles),
            "split": int(split["split"]),
            "split_source": str(split.get("split_source", "yolo-fixed")),
            **({
                "planner_selected_cut_after_node":
                int(split["planner_selected_cut_after_node"])
            } if split.get("planner_selected_cut_after_node") is not None else {}),
            **({
                "planner_selected_node": str(split["planner_selected_node"])
            } if split.get("planner_selected_node") else {}),
            **({
                "planner_selected_score": float(split["planner_selected_score"])
            } if split.get("planner_selected_score") is not None else {}),
            "sharding": f"{layout_semantics}-{layout}",
            "dependency_source": "onnx-chunk-io",
            "full_onnx_model": str(split.get("full_model_path", "")),
            "onnx_graph_summary": str(split.get("onnx_graph_summary", "")),
            "onnx_split_candidate_count": len(split.get("onnx_split_candidates") or []),
            "planner_recommendation_count": len(split.get("planner_recommendations") or []),
            **({
                "planner_cost_summary": split["planner_cost_summary"]
            } if split.get("planner_cost_summary") else {}),
            **({
                "planner_compute_summary": split["planner_compute_summary"]
            } if split.get("planner_compute_summary") else {}),
            **({
                "planner_candidate_scores": split["planner_candidate_scores"]
            } if split.get("planner_candidate_scores") else {}),
            **({
                "planner_selected_candidate": split["planner_selected_candidate"]
            } if split.get("planner_selected_candidate") else {}),
        },
    )
    compute_providers = compute_provider_identities(len(roles))
    repo_service = SplitServiceSpec(
        name=REPO_SERVICE,
        model_name=REPO_SERVICE,
        roles=[],
        dependencies=[],
        users=[CONTROLLER, USER, *compute_providers],
        providers=[{"identity": REPO_PROVIDER, "roles": []}],
    )
    return SplitterOutput(
        application=f"yolo-{layout}-demo",
        controller=CONTROLLER,
        group=GROUP,
        user=USER,
        provider_prefix=PROVIDER_PREFIX,
        services=[service, repo_service],
        provider_identities=compute_providers,
        trust_app_roots=["/example"],
        metadata=service.metadata,
    )


def yolo_dynamic_splitter_output(split: dict, *, trust_anchor_file: str = "") -> SplitterOutput:
    output = yolo_splitter_output(split)
    return SplitterOutput(
        application=output.application,
        controller=output.controller,
        group=output.group,
        user=output.user,
        provider_prefix=output.provider_prefix,
        services=output.services,
        provider_identities=output.provider_identities,
        trust_app_roots=output.trust_app_roots,
        trust_anchor_file=trust_anchor_file,
        artifact_allowlist=[RUNTIME_NAME],
        artifact_sandbox={
            "kind": "local-python",
            "command": ["python3"],
        },
        metadata=output.metadata,
    )


def build_runner_script() -> bytes:
    return b'''#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[1] != "--probe":
        print("usage: yolo_2x2_runner.py --probe <role> <model-path>", file=sys.stderr)
        return 2
    role = sys.argv[2]
    model_path = Path(sys.argv[3])
    digest = hashlib.sha256(model_path.read_bytes()).hexdigest()[:16]
    print(f"YOLO_2X2_DOWNLOADED_RUNNER role={role} modelSha256={digest}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
'''


def runtime_spec() -> RuntimeSpec:
    return RuntimeSpec(
        name=RUNTIME_NAME,
        backend="onnxruntime",
        entrypoint="runner",
        artifact=ArtifactSpec(
            name="runner",
            payload=build_runner_script(),
            filename="yolo_2x2_runner.py",
            kind="runtime-script",
            executable=True,
            cache_name=RUNTIME_NAME,
        ),
    )


def build_dynamic_plan(client):
    service_name = yolo_inference_service(client.deployment)
    service_policy = client.deployment.service_policy(service_name)
    runtime = runtime_spec()
    builder = client.plan_builder(service_name, runtime=runtime, backend="onnxruntime")
    artifacts = {artifact.role: artifact for artifact in service_policy.artifacts}
    for role in service_policy.roles:
        artifact = artifacts[role]
        builder.add_part(
            role=role,
            model=artifact.path,
            artifact_name=artifact.artifact_name,
            filename=artifact.filename,
            kind=artifact.kind,
            backend=artifact.backend or "onnxruntime",
            runtime=runtime,
            cache_name=artifact.artifact_name,
            allow_dynamic_provisioning=True,
        )
    return builder.build()


def load_repo_manifests(path: str | Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def build_repo_plan(client, manifest_path: str | Path):
    manifests = load_repo_manifests(manifest_path)
    service_name = yolo_inference_service(client.deployment)
    service_policy = client.deployment.service_policy(service_name)
    runtime = runtime_spec()
    builder = client.plan_builder(service_name, runtime=runtime, backend="onnxruntime")
    artifacts = {artifact.role: artifact for artifact in service_policy.artifacts}
    for role in service_policy.roles:
        artifact = artifacts[role]
        role_manifests = manifests["roles"][role]
        runner_manifest = repo_manifest_from_large_data_reference(role_manifests["runner"])
        model_manifest = repo_manifest_from_large_data_reference(role_manifests["model"])
        builder.add_part(
            role=role,
            model=b"",
            artifact_name=artifact.artifact_name,
            filename=artifact.filename,
            kind=artifact.kind,
            backend=artifact.backend or "onnxruntime",
            runtime=RuntimeSpec(
                name=runtime.name,
                backend=runtime.backend,
                entrypoint=runtime.entrypoint,
                artifact=ArtifactSpec(
                    name="runner",
                    payload=b"",
                    filename=runtime.artifact.filename,
                    kind=runtime.artifact.kind,
                    executable=runtime.artifact.executable,
                    cache_name=runtime.artifact.cache_name,
                    repo_manifest=runner_manifest,
                ),
            ),
            cache_name=artifact.artifact_name,
            repo_manifest=model_manifest,
            allow_dynamic_provisioning=True,
        )
    return builder.build()


def encode_image_for_yolo(image: np.ndarray) -> bytes:
    return _npz_payload({"images": image.astype(np.float32)})


def decode_image(payload: bytes) -> np.ndarray:
    return _load_npz_payload(payload)["images"].astype(np.float32)


def encode_image_reference(data_name: str, payload: bytes) -> bytes:
    return encode_large_data_reference_payload(LargeDataReference(
        data_name=data_name,
        object_type="application/x-ndnsf-di-input+npz",
        object_id="inference-input-image",
        plaintext_size=len(payload),
        encrypted=True,
        digest="sha256:" + hashlib.sha256(payload).hexdigest(),
    ))


def decode_image_reference(payload: bytes) -> dict:
    ref = parse_large_data_reference_payload(payload)
    if ref is None:
        raise ValueError("request payload is not an NDNSF large-data reference")
    digest = ref.digest
    if digest.startswith("sha256:"):
        digest = digest[len("sha256:"):]
    return {
        "data_name": ref.data_name,
        "size": ref.plaintext_size,
        "sha256": digest,
        "object_type": ref.object_type,
        "object_id": ref.object_id,
        "encrypted": ref.encrypted,
    }


def verify_referenced_payload(ref: dict, payload: bytes) -> None:
    expected_size = int(ref.get("size", -1))
    if expected_size >= 0 and len(payload) != expected_size:
        raise ValueError(
            f"referenced payload size mismatch: expected={expected_size} actual={len(payload)}")
    expected_hash = str(ref.get("sha256", ""))
    if expected_hash and hashlib.sha256(payload).hexdigest() != expected_hash:
        raise ValueError("referenced payload SHA-256 mismatch")


def encode_binary_payload(offset: int, payload: bytes) -> bytes:
    return _npz_payload({
        "offset": np.array(offset, dtype=np.int64),
        "payload": np.frombuffer(payload, dtype=np.uint8),
    })


def decode_binary_payload(payload: bytes) -> tuple[int, bytes]:
    obj = _load_npz_payload(payload)
    return int(obj["offset"]), obj["payload"].astype(np.uint8).tobytes()


def encode_yolo_output(offset: int, value: np.ndarray) -> bytes:
    return _npz_payload({
        "offset": np.array(offset, dtype=np.int64),
        "output": value.astype(np.float32),
    })


def decode_yolo_output(payload: bytes) -> tuple[int, np.ndarray]:
    if payload.startswith(b"NDITB001"):
        tensors = _decode_native_tensor_bundle(payload)
        if "output" in tensors:
            return 0, tensors["output"]
        if "predictions" in tensors:
            return 0, tensors["predictions"]
        if len(tensors) == 1:
            return 0, next(iter(tensors.values()))
        raise KeyError("native YOLO output bundle does not contain output/predictions")
    obj = _load_npz_payload(payload)
    return int(obj["offset"]), obj["output"].astype(np.float32)


def make_ort_session(path: str | Path) -> ort.InferenceSession:
    return ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])


def _value_for_input(values: dict[str, np.ndarray], name: str) -> np.ndarray:
    if name in values:
        return values[name]
    base, dot, suffix = name.rpartition(".")
    if dot and suffix.isdigit() and base in values:
        return values[base]
    raise KeyError(name)


def _run_onnx_to_npz(model_path: str | Path, values: dict[str, np.ndarray]) -> bytes:
    session = make_ort_session(model_path)
    feed = {
        input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
        for input_info in session.get_inputs()
    }
    outputs = session.run(None, feed)
    next_values = {
        output.name: np.asarray(value, dtype=np.float32)
        for output, value in zip(session.get_outputs(), outputs)
    }
    return npz_payload(next_values)


def run_intermediate_chunk(model_path: str | Path, input_payload: bytes | np.ndarray,
                           *, image_input: bool = False) -> bytes:
    if image_input:
        values = {"images": np.asarray(input_payload, dtype=np.float32)}
    else:
        values = load_npz_payload(input_payload)
    return _run_onnx_to_npz(model_path, values)


def run_final_chunk(model_path: str | Path, input_payload: bytes) -> np.ndarray:
    payload = _run_onnx_to_npz(model_path, load_npz_payload(input_payload))
    values = load_npz_payload(payload)
    return values.get("predictions", next(iter(values.values()))).astype(np.float32)


def run_local_onnx_pipeline(model_paths: dict[str, str | Path],
                            image: np.ndarray,
                            roles: Sequence[str] | None = None) -> np.ndarray:
    ordered_roles = list(roles or model_paths.keys())
    if not ordered_roles:
        raise ValueError("run_local_onnx_pipeline requires at least one role")
    payload = run_intermediate_chunk(model_paths[ordered_roles[0]], image, image_input=True)
    for role in ordered_roles[1:-1]:
        payload = run_intermediate_chunk(model_paths[role], payload)
    if len(ordered_roles) == 1:
        values = load_npz_payload(payload)
        return values.get("predictions", next(iter(values.values()))).astype(np.float32)
    return run_final_chunk(model_paths[ordered_roles[-1]], payload)


def run_local_parallel_output_pipeline(model_paths: dict[str, str | Path],
                                       image: np.ndarray,
                                       layout: str = DEFAULT_LAYOUT) -> np.ndarray:
    layout = normalize_layout(layout)
    stages, shards = parse_layout(layout)
    values: dict[str, np.ndarray] = {}
    for shard in range(shards):
        role = f"/Stage/0/Shard/{shard}"
        payload = run_intermediate_chunk(model_paths[role], image, image_input=True)
        values.update(load_npz_payload(payload))
    for stage in range(1, stages):
        for shard in range(shards):
            role = f"/Stage/{stage}/Shard/{shard}"
            input_name = f"stage{stage - 1}_shard_{shard}"
            payload = _run_onnx_to_npz(model_paths[role], {
                input_name: values[input_name],
            })
            values.update(load_npz_payload(payload))
    merge_inputs = {
        f"stage{stages - 1}_shard_{shard}":
        values[f"stage{stages - 1}_shard_{shard}"]
        for shard in range(shards)
    }
    payload = _run_onnx_to_npz(model_paths[ROLE_MERGE], merge_inputs)
    merged = load_npz_payload(payload)
    return merged.get("predictions", next(iter(merged.values()))).astype(np.float32)


def run_local_parallel_detect_scale_pipeline(model_paths: dict[str, str | Path],
                                             image: np.ndarray,
                                             layout: str = DEFAULT_LAYOUT) -> np.ndarray:
    layout = normalize_layout(layout)
    _, shards = parse_layout(layout)
    values: dict[str, np.ndarray] = {}
    has_shared_backbone = ROLE_BACKBONE in model_paths
    if has_shared_backbone:
        payload = run_intermediate_chunk(model_paths[ROLE_BACKBONE], image, image_input=True)
        values.update(load_npz_payload(payload))
    for shard in range(shards):
        role = f"/Head/Shard/{shard}"
        session = make_ort_session(model_paths[role])
        if has_shared_backbone:
            feed = {
                input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
                for input_info in session.get_inputs()
            }
        else:
            feed = {
                input_info.name: image.astype(np.float32)
                for input_info in session.get_inputs()
            }
        outputs = session.run(None, feed)
        values.update({
            output.name: np.asarray(value, dtype=np.float32)
            for output, value in zip(session.get_outputs(), outputs)
        })
    merge_session = make_ort_session(model_paths[ROLE_MERGE])
    merge_feed = {
        input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
        for input_info in merge_session.get_inputs()
    }
    outputs = merge_session.run(None, merge_feed)
    merged = {
        output.name: np.asarray(value, dtype=np.float32)
        for output, value in zip(merge_session.get_outputs(), outputs)
    }
    return merged.get("predictions", next(iter(merged.values()))).astype(np.float32)


def parse_args_with_common(description: str):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--group", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true")
    return parser
