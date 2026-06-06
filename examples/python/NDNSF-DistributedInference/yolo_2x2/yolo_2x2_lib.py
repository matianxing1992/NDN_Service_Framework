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
import subprocess
import sys
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
from ndnsf_distributed_inference.plan import ArtifactSpec, RuntimeSpec
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


def _estimated_segments(byte_count: int) -> int:
    if byte_count <= 0:
        return 0
    estimated_wire_bytes = int(byte_count * 1.5) + 4096
    return max(1, (estimated_wire_bytes + 6999) // 7000)


def split_model(output_dir: str | Path,
                model_name: str = DEFAULT_MODEL,
                input_size: int = DEFAULT_INPUT_SIZE,
                provider_profiles: list[ProviderProfile] | None = None,
                auto_split: bool = False,
                layout: str = DEFAULT_LAYOUT,
                parallel_output_shards: bool = False,
                parallel_detect_scale_shards: bool = False) -> dict:
    if parallel_detect_scale_shards:
        return split_parallel_detect_scale_model(
            output_dir,
            model_name=model_name,
            input_size=input_size,
            provider_profiles=provider_profiles,
            auto_split=auto_split,
            layout=layout,
        )
    if parallel_output_shards:
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

    x = torch.from_numpy(make_input(input_size)).float()
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
        current_saved = output_saved

    dependencies, chunk_graph = _build_yolo_onnx_dependencies(paths, roles=roles)
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
                                      layout: str = DEFAULT_LAYOUT) -> dict:
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
        def __init__(self, detect, scale_indices: Sequence[int]):
            super().__init__()
            heads = detect.one2one if getattr(detect, "end2end", False) else detect.one2many
            self.box_head = heads["box_head"]
            self.cls_head = heads["cls_head"]
            self.scale_indices = [int(index) for index in scale_indices]
            self.reg_max = int(detect.reg_max)
            self.nc = int(detect.nc)

        def forward(self, *features):
            outputs = []
            for scale, feature in zip(self.scale_indices, features):
                batch = feature.shape[0]
                boxes = self.box_head[scale](feature).view(batch, 4 * self.reg_max, -1)
                scores = self.cls_head[scale](feature).view(batch, self.nc, -1)
                outputs.extend([boxes, scores])
            return tuple(outputs)

    class YoloDetectMerge(nn.Module):
        def __init__(self, detect):
            super().__init__()
            self.detect = detect
            self.register_buffer("anchors_const", detect.anchors.detach().clone())
            self.register_buffer("strides_const", detect.strides.detach().clone())

        def forward(self, *values):
            boxes = torch.cat(tuple(values[0::2]), dim=-1)
            scores = torch.cat(tuple(values[1::2]), dim=-1)
            dbox = self.detect.decode_bboxes(
                self.detect.dfl(boxes),
                self.anchors_const.unsqueeze(0),
            ) * self.strides_const
            y = torch.cat((dbox, scores.sigmoid()), 1)
            if getattr(self.detect, "end2end", False):
                return self.detect.postprocess(y.permute(0, 2, 1))
            return y

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
    roles = parallel_detect_scale_roles_for_layout(layout)
    head_roles = [f"/Head/Shard/{index}" for index in range(len(scale_groups))]
    roles = [ROLE_BACKBONE, *head_roles, ROLE_MERGE]
    stem = Path(loaded_name).stem
    full_model_path = output / f"{stem}-full-{input_size}.onnx"
    paths: dict[str, Path] = {}
    chunk_metadata: dict[str, dict] = {}

    x = torch.from_numpy(make_input(input_size)).float()
    with torch.no_grad():
        expected = YoloFull(model).eval()(x)
        feature_values = YoloBackboneFeatures(model, feature_indices).eval()(x)

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

    backbone_path = output / f"{stem}-Backbone-{input_size}.onnx"
    feature_tensor_names = [
        f"detect_feature_{index}"
        for index in range(len(feature_indices))
    ]
    if not backbone_path.exists():
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
    chunk_metadata[ROLE_BACKBONE] = {
        "source_model": loaded_name,
        "input_size": input_size,
        "layout": layout,
        "layout_semantics": YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
        "role_type": "shared-backbone-neck",
        "feature_indices": feature_indices,
        "output_tensors": feature_tensor_names,
        "final": False,
    }

    head_outputs: dict[str, torch.Tensor] = {}
    for shard, group in enumerate(scale_groups):
        role = f"/Head/Shard/{shard}"
        input_names = [feature_tensor_names[index] for index in group]
        output_names = [
            name
            for scale in group
            for name in (f"boxes_scale_{scale}", f"scores_scale_{scale}")
        ]
        input_values = tuple(feature_values[index] for index in group)
        with torch.no_grad():
            outputs = YoloDetectHeadShard(detect, group).eval()(*input_values)
        for name, value in zip(output_names, outputs):
            head_outputs[name] = value
        path = output / f"{stem}-{role.strip('/').replace('/', '-')}-{input_size}.onnx"
        if not path.exists():
            torch.onnx.export(
                YoloDetectHeadShard(detect, group).eval(),
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
            "layout_semantics": YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
            "role_type": "detect-head-scale-shard",
            "shard": shard,
            "scale_indices": group,
            "input_tensors": input_names,
            "output_tensors": output_names,
            "final": False,
        }

    merge_inputs = [
        name
        for scale in range(len(feature_indices))
        for name in (f"boxes_scale_{scale}", f"scores_scale_{scale}")
    ]
    merge_values = tuple(head_outputs[name] for name in merge_inputs)
    merge_path = output / f"{stem}-DetectMerge-{layout}-{input_size}.onnx"
    if not merge_path.exists():
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
    chunk_metadata[ROLE_MERGE] = {
        "source_model": loaded_name,
        "input_size": input_size,
        "layout": layout,
        "layout_semantics": YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
        "role_type": "detect-merge-decode",
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
        "layout_semantics": YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
        "stage_shards_parallel": True,
        "dependencies": [],
    }
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

    merge_bytes = sum(_tensor_nbytes(value) for value in head_outputs.values())
    merge_dep = InferenceDependency(
        producers=head_roles,
        consumers=[ROLE_MERGE],
        key_scope="detect-heads-to-merge",
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
        "layout_semantics": YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
        "service": service_name_for_layout(layout),
        "roles": roles,
        "stage_roles": head_roles,
        "split": 0,
        "split_source": YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
        "chunks": chunk_metadata,
        "dependencies": dependencies,
        "onnx_graph_summary": graph_summary,
        "onnx_split_candidates": split_candidates,
        "planner_recommendations": planner_recommendations,
        "reference_output_shape": list(expected.shape),
    }


def default_planner_provider_profiles() -> list[ProviderProfile]:
    return homogeneous_provider_profiles([
        "/NDNSF-DistributeInference/example/provider/A",
        "/NDNSF-DistributeInference/example/provider/B",
    ])


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
        },
    )
    repo_service = SplitServiceSpec(
        name=REPO_SERVICE,
        model_name=REPO_SERVICE,
        roles=[],
        dependencies=[],
        users=[CONTROLLER, USER],
        providers=[{"identity": REPO_PROVIDER, "roles": []}],
    )
    return SplitterOutput(
        application=f"yolo-{layout}-demo",
        controller=CONTROLLER,
        group=GROUP,
        user=USER,
        provider_prefix=PROVIDER_PREFIX,
        services=[service, repo_service],
        provider_identities=compute_provider_identities(len(roles)),
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
    payload = run_intermediate_chunk(model_paths[ROLE_BACKBONE], image, image_input=True)
    values.update(load_npz_payload(payload))
    for shard in range(shards):
        role = f"/Head/Shard/{shard}"
        session = make_ort_session(model_paths[role])
        feed = {
            input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
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
