"""Shared code for the Python YOLO split-collaboration example."""

from __future__ import annotations

import gzip
import io
import json
import re
import shutil
import subprocess
from contextlib import contextmanager
from functools import lru_cache
from pathlib import Path
from typing import Any, Iterator

import numpy as np
import onnxruntime as ort
import torch
import torch.nn as nn
import yaml

from ndnsf_distributed_inference import (
    InferenceDependency,
    ProviderProfile,
    SequentialSplitCandidate,
    analyze_onnx_graph,
    estimate_split_candidates,
    homogeneous_provider_profiles,
    recommend_sequential_splits,
    write_onnx_graph_summary,
)
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)


SERVICE = "/AI/YOLO/SplitInference"
GROUP = "/example/hello/group"
CONTROLLER = "/example/hello/controller"
USER = "/example/hello/user"
PROVIDER_PREFIX = "/example/hello/provider"
POLICY_FILE = "examples/yolo_collaboration.policies"
TRUST_SCHEMA = "examples/trust-schema.conf"
CONFIG_FILE = str(Path(__file__).with_name("yolo_policy.yaml"))
ROLE_STAGE0 = "/Stage/0"
ROLE_STAGE1 = "/Stage/1"
ROLE_TO_RUNNER_STAGE = {
    ROLE_STAGE0: "stage0",
    ROLE_STAGE1: "stage1",
}

DEFAULT_MODEL_CANDIDATES = ("yolo26n.pt", "yolo11n.pt")
DEFAULT_INPUT_SIZE = 32


@contextmanager
def optional_local_nfd(enabled: bool) -> Iterator[None]:
    """Optionally start local NFD for a single-host example run.

    This helper stays local to the example, so installed scripts do not need to
    discover the NDNSF source tree or import shared helpers from examples/.
    """

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


@lru_cache(maxsize=2)
def load_yolo_model(preferred: str = "yolo26n.pt"):
    from ultralytics import YOLO

    last_error: Exception | None = None
    candidates = (preferred,) + tuple(
        name for name in DEFAULT_MODEL_CANDIDATES if name != preferred)
    for name in candidates:
        try:
            model = YOLO(name).model.eval()
            model.cpu()
            return name, model
        except Exception as exc:
            last_error = exc
    raise RuntimeError(f"failed to load YOLO model: {last_error}")


def split_index(model) -> int:
    # Keep stage0 small enough for a practical local NDNSF artifact smoke test
    # while still making stage1 consume an intermediate tensor produced by a
    # different provider. The API is generic; applications can choose a deeper
    # split when the artifact repository/cache path is used.
    return min(6, len(model.model) // 2)


class YoloStage0(nn.Module):
    def __init__(self, model, split: int, saved_indices: list[int]):
        super().__init__()
        self.model = model
        self.split = split
        self.saved_indices = saved_indices

    def forward(self, x):
        saved = []
        x, saved = run_modules(self.model, 0, self.split, x, saved)
        return tuple([x] + [saved[index] for index in self.saved_indices])


class YoloStage1(nn.Module):
    def __init__(self, model, split: int, saved_indices: list[int]):
        super().__init__()
        self.model = model
        self.split = split
        self.saved_indices = saved_indices

    def forward(self, x, *saved_values):
        saved = [None] * self.split
        for index, value in zip(self.saved_indices, saved_values):
            saved[index] = value
        out, _ = run_modules(self.model, self.split, len(self.model.model), x, saved)
        return first_tensor(out)


class YoloFull(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, x):
        return first_tensor(self.model(x))


@torch.no_grad()
def run_modules(model, start: int, end: int, x, saved):
    modules = model.model
    for i in range(start, end):
        module = modules[i]
        if module.f == -1:
            module_input = x
        elif isinstance(module.f, int):
            module_input = saved[module.f]
        else:
            module_input = [x if j == -1 else saved[j] for j in module.f]
        x = module(module_input)
        saved.append(x if module.i in model.save else None)
    return x, saved


def first_tensor(value):
    if torch.is_tensor(value):
        return value
    if isinstance(value, (list, tuple)):
        for item in value:
            tensor = first_tensor(item)
            if tensor is not None:
                return tensor
    return None


def tensor_payload(value: Any) -> bytes:
    buffer = io.BytesIO()
    torch.save(value, buffer)
    return gzip.compress(buffer.getvalue(), compresslevel=9)


def npz_payload(values: dict[str, np.ndarray]) -> bytes:
    buffer = io.BytesIO()
    np.savez_compressed(buffer, **values)
    return buffer.getvalue()


def load_npz_payload(payload: bytes) -> dict[str, np.ndarray]:
    with np.load(io.BytesIO(payload)) as data:
        return {name: data[name] for name in data.files}


def load_tensor_payload(payload: bytes):
    data = gzip.decompress(payload)
    kwargs = {"map_location": "cpu"}
    try:
        kwargs["weights_only"] = True
        return torch.load(io.BytesIO(data), **kwargs)
    except TypeError:
        kwargs.pop("weights_only", None)
        return torch.load(io.BytesIO(data), **kwargs)


def make_input(size: int = DEFAULT_INPUT_SIZE) -> torch.Tensor:
    torch.manual_seed(20260527)
    return torch.rand(1, 3, size, size)


def encode_initial_request(x: torch.Tensor) -> bytes:
    return npz_payload({"images": x.float().cpu().numpy().astype(np.float16)})


def decode_initial_request(payload: bytes) -> torch.Tensor:
    return torch.from_numpy(load_npz_payload(payload)["images"].astype(np.float32)).float()


def decode_initial_request_np(payload: bytes) -> np.ndarray:
    return load_npz_payload(payload)["images"].astype(np.float32)


def encode_activation(x, saved) -> bytes:
    return tensor_payload({
        "x": x.float().cpu(),
        "saved": [None if value is None else value.float().cpu() for value in saved],
    })


def decode_activation(payload: bytes):
    obj = load_tensor_payload(payload)
    return (
        obj["x"].float(),
        [None if value is None else value.float() for value in obj["saved"]],
    )


def encode_output(value) -> bytes:
    return tensor_payload({"output": first_tensor(value).float().cpu()})


def decode_output(payload: bytes) -> torch.Tensor:
    return load_tensor_payload(payload)["output"].float()


def encode_onnx_output(output: np.ndarray) -> bytes:
    return npz_payload({"output": output.astype(np.float32)})


def decode_onnx_output(payload: bytes) -> np.ndarray:
    return load_npz_payload(payload)["output"].astype(np.float32)


def full_forward_from_policy_onnx(config_path: str | Path, x: torch.Tensor) -> np.ndarray | None:
    """Run the full ONNX model recorded by an auto-split policy, if present."""

    data = yaml.safe_load(Path(config_path).read_text(encoding="utf-8"))
    for service in data.get("services", []):
        metadata_candidates = [service.get("metadata") or {}]
        metadata_candidates.extend(
            (artifact.get("metadata") or {})
            for artifact in service.get("artifacts", [])
        )
        full_model_path = next(
            (metadata.get("full_onnx_model")
             for metadata in metadata_candidates
             if metadata.get("full_onnx_model")),
            None,
        )
        if not full_model_path:
            continue
        session = make_ort_session(full_model_path)
        output = session.run(None, {"images": x.float().cpu().numpy().astype(np.float32)})[0]
        return np.asarray(output, dtype=np.float32)
    return None


@torch.no_grad()
def full_forward(model, x: torch.Tensor) -> torch.Tensor:
    model.eval()
    return first_tensor(model(x.float())).float().cpu()


def export_yolo_split_onnx(
    preferred: str,
    output_dir: str | Path,
    input_size: int = DEFAULT_INPUT_SIZE,
    opset: int = 17,
    auto_split: bool = False,
    provider_profiles: list[ProviderProfile] | None = None,
) -> dict:
    """Export stage0 and stage1 ONNX models for the selected YOLO nano model."""

    model_name, model = load_yolo_model(preferred)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    stem = Path(model_name).stem
    x = make_input(input_size).float()

    split_metadata: dict[str, Any] = {
        "split_source": "yolo-fixed",
    }
    if auto_split:
        split, split_metadata = _planner_selected_split(
            model=model,
            model_name=model_name,
            output_dir=output,
            input_size=input_size,
            sample_input=x,
            opset=opset,
            provider_profiles=provider_profiles or default_planner_provider_profiles(),
        )
    else:
        split = split_index(model)

    saved_indices = [int(index) for index in model.save if int(index) < split]
    stage0_path = output / f"{stem}-stage0-{input_size}.onnx"
    stage1_path = output / f"{stem}-stage1-{input_size}.onnx"

    with torch.no_grad():
        stage0_outputs = YoloStage0(model, split, saved_indices)(x)

    if not stage0_path.exists():
        torch.onnx.export(
            YoloStage0(model, split, saved_indices),
            x,
            str(stage0_path),
            input_names=["images"],
            output_names=["x"] + [f"saved_{index}" for index in saved_indices],
            opset_version=opset,
            do_constant_folding=True,
        )

    if not stage1_path.exists():
        torch.onnx.export(
            YoloStage1(model, split, saved_indices),
            tuple(stage0_outputs),
            str(stage1_path),
            input_names=["x"] + [f"saved_{index}" for index in saved_indices],
            output_names=["predictions"],
            opset_version=opset,
            do_constant_folding=True,
        )

    return {
        "model": model_name,
        "split": split,
        "saved_indices": saved_indices,
        "input_size": input_size,
        "stage0_path": stage0_path,
        "stage1_path": stage1_path,
        **split_metadata,
    }


def default_planner_provider_profiles() -> list[ProviderProfile]:
    return homogeneous_provider_profiles([
        "/example/hello/provider",
        "/example/hello/provider/A",
    ])


def load_provider_profiles(path: str | Path) -> list[ProviderProfile]:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if isinstance(data, dict):
        data = data.get("providers", [])
    if not isinstance(data, list):
        raise ValueError("provider profile file must contain a list or {providers: [...]}")
    return [ProviderProfile.from_dict(item) for item in data]


def _planner_selected_split(
    *,
    model,
    model_name: str,
    output_dir: Path,
    input_size: int,
    sample_input: torch.Tensor,
    opset: int,
    provider_profiles: list[ProviderProfile],
) -> tuple[int, dict[str, Any]]:
    stem = Path(model_name).stem
    full_model_path = output_dir / f"{stem}-full-{input_size}.onnx"
    if not full_model_path.exists():
        torch.onnx.export(
            YoloFull(model).eval(),
            sample_input,
            str(full_model_path),
            input_names=["images"],
            output_names=["predictions"],
            opset_version=opset,
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
        providers=provider_profiles,
        max_recommendations=10,
    )
    if not planner_recommendations:
        return split_index(model), {"split_source": "yolo-fixed-fallback"}

    selected = planner_recommendations[0]
    split = _module_split_from_cut(
        full_summary.nodes[selected.candidate.cut_after_node].name,
        module_count=len(model.model),
        fallback=split_index(model),
    )
    summary_path = output_dir / f"{stem}-2stage-onnx-graph-summary.json"
    write_onnx_graph_summary(
        summary_path,
        full_model_summary=full_summary,
        split_candidates=split_candidates,
        planner_recommendations=planner_recommendations,
    )
    return split, {
        "split_source": "onnx-planner",
        "full_model_path": full_model_path,
        "onnx_graph_summary": summary_path,
        "planner_selected_cut_after_node": selected.candidate.cut_after_node,
        "planner_selected_node": full_summary.nodes[selected.candidate.cut_after_node].name,
        "planner_selected_score": selected.score,
        "planner_selected_transfer_ms": selected.transfer_ms,
        "planner_selected_compute_imbalance": selected.compute_imbalance,
    }


def _module_split_from_cut(node_name: str, *, module_count: int, fallback: int) -> int:
    match = re.search(r"/model(?:/model)?\.(\d+)(?:/|$)", node_name)
    if not match:
        return fallback
    module_index = int(match.group(1))
    return max(1, min(module_count - 1, module_index + 1))


def yolo_splitter_output(exported: dict) -> SplitterOutput:
    """Build NDNSF-DistributedInference policy input from YOLO split output.

    This is the example-specific splitter boundary: the YOLO splitter knows the
    two ONNX stages and their dependency; NDNSF-DistributedInference consumes
    the standard SplitterOutput without inferring model internals.
    """

    model_name = str(exported["model"])
    input_size = int(exported["input_size"])
    common_metadata = {
        "source_model": model_name,
        "split": int(exported["split"]),
        "saved_indices": [int(value) for value in exported["saved_indices"]],
        "input_size": input_size,
        "split_source": str(exported.get("split_source", "yolo-fixed")),
        **({"full_onnx_model": str(exported["full_model_path"])}
           if exported.get("full_model_path") else {}),
        **({"onnx_graph_summary": str(exported["onnx_graph_summary"])}
           if exported.get("onnx_graph_summary") else {}),
        **({"planner_selected_cut_after_node":
            int(exported["planner_selected_cut_after_node"])}
           if exported.get("planner_selected_cut_after_node") is not None else {}),
        **({"planner_selected_node": str(exported["planner_selected_node"])}
           if exported.get("planner_selected_node") else {}),
        **({"planner_selected_score": float(exported["planner_selected_score"])}
           if exported.get("planner_selected_score") is not None else {}),
        **({"planner_selected_transfer_ms":
            float(exported["planner_selected_transfer_ms"])}
           if exported.get("planner_selected_transfer_ms") is not None else {}),
        **({"planner_selected_compute_imbalance":
            float(exported["planner_selected_compute_imbalance"])}
           if exported.get("planner_selected_compute_imbalance") is not None else {}),
    }
    service = SplitServiceSpec(
        name=SERVICE,
        model_name="/Model/Ultralytics/YOLO/Split",
        roles=[ROLE_STAGE0, ROLE_STAGE1],
        dependencies=[
            InferenceDependency(
                producers=[ROLE_STAGE0],
                consumers=[ROLE_STAGE1],
                key_scope="stage0-to-stage1",
                topic_prefix="/activation",
                tensors=["x"] + [f"saved_{index}" for index in exported["saved_indices"]],
            ),
        ],
        artifacts=[
            SplitArtifact(
                role=ROLE_STAGE0,
                artifact_name="/Model/Ultralytics/YOLO/Stage/0",
                path=str(exported["stage0_path"]),
                kind="onnx-model",
                backend="onnxruntime",
                metadata=dict(common_metadata, runner_stage="stage0"),
            ),
            SplitArtifact(
                role=ROLE_STAGE1,
                artifact_name="/Model/Ultralytics/YOLO/Stage/1",
                path=str(exported["stage1_path"]),
                kind="onnx-model",
                backend="onnxruntime",
                metadata=dict(common_metadata, runner_stage="stage1"),
            ),
        ],
        input_schema={
            "codec": "npz",
            "encoder": "encode_initial_request(image_tensor)",
            "fields": {
                "images": {
                    "dtype": "float16",
                    "shape": [1, 3, input_size, input_size],
                    "layout": "NCHW",
                },
            },
        },
        output_schema={
            "codec": "npz",
            "decoder": "decode_onnx_output(response.payload)",
            "fields": {
                "output": {
                    "dtype": "float32",
                },
            },
        },
        metadata=common_metadata,
    )
    return SplitterOutput(
        application="yolo-split-demo",
        controller=CONTROLLER,
        group=GROUP,
        user=USER,
        provider_prefix=PROVIDER_PREFIX,
        services=[service],
        trust_app_roots=["/example"],
        metadata=common_metadata,
    )


def make_ort_session(path: str | Path) -> ort.InferenceSession:
    return ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])


def run_onnx_stage0(model_path: str | Path, request_payload: bytes) -> bytes:
    session = make_ort_session(model_path)
    images = decode_initial_request_np(request_payload)
    outputs = session.run(None, {"images": images})
    values = {
        output.name: np.asarray(value, dtype=np.float32)
        for output, value in zip(session.get_outputs(), outputs)
    }
    return npz_payload(values)


def run_onnx_stage1(model_path: str | Path, activation_payload: bytes) -> bytes:
    session = make_ort_session(model_path)
    activation = load_npz_payload(activation_payload)
    feed = {
        input_info.name: activation[input_info.name].astype(np.float32)
        for input_info in session.get_inputs()
    }
    output = session.run(None, feed)[0]
    return encode_onnx_output(np.asarray(output, dtype=np.float32))
