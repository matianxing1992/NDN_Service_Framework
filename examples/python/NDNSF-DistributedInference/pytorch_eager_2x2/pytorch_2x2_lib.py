"""PyTorch-defined fully connected ONNX 2x2 example helpers.

The model is authored in PyTorch for readability, exported to ONNX, and then
split into four ONNX shards. This keeps the example close to common AI
developer workflows while exercising the portable ONNX runtime path.
"""

from __future__ import annotations

import argparse
import io
import shutil
import subprocess
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

import numpy as np
import onnxruntime as ort

from ndnsf_distributed_inference import InferenceDependency
from ndnsf_distributed_inference.onnx_graph import (
    OnnxChunkSpec,
    analyze_onnx_graph,
    build_sequential_chunk_dependencies,
    estimate_split_candidates,
    write_onnx_graph_summary,
)
from ndnsf_distributed_inference.split_planner import (
    SequentialSplitCandidate,
    homogeneous_provider_profiles,
    recommend_sequential_splits,
)
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)


SERVICE = "/AI/PyTorch/FcOnnx2x2Inference"
GROUP = "/NDNSF-DistributeInference/example/group"
CONTROLLER = "/NDNSF-DistributeInference/example/controller"
USER = "/NDNSF-DistributeInference/example/user"
PROVIDER_PREFIX = "/NDNSF-DistributeInference/example/provider"
CONFIG_FILE = str(Path(__file__).with_name("pytorch_policy.yaml"))

ROLE_S0_0 = "/Stage/0/Shard/0"
ROLE_S0_1 = "/Stage/0/Shard/1"
ROLE_S1_0 = "/Stage/1/Shard/0"
ROLE_S1_1 = "/Stage/1/Shard/1"
ROLES = [ROLE_S0_0, ROLE_S0_1, ROLE_S1_0, ROLE_S1_1]

INPUT_DIM = 8
HIDDEN_DIM = 8
OUTPUT_DIM = 4


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


def tensor_payload(value: dict[str, np.ndarray | int]) -> bytes:
    buffer = io.BytesIO()
    arrays = {
        key: np.asarray(item)
        for key, item in value.items()
    }
    np.savez_compressed(buffer, **arrays)
    return buffer.getvalue()


def load_tensor_payload(payload: bytes) -> dict[str, np.ndarray]:
    with np.load(io.BytesIO(payload), allow_pickle=False) as data:
        return {key: np.asarray(data[key]) for key in data.files}


def make_input() -> np.ndarray:
    rng = np.random.default_rng(20260528)
    return rng.standard_normal((1, INPUT_DIM), dtype=np.float32)


def make_full_model() -> dict[str, np.ndarray]:
    rng = np.random.default_rng(20260528)
    return {
        "w0": rng.standard_normal((HIDDEN_DIM, INPUT_DIM), dtype=np.float32) * 0.2,
        "b0": rng.standard_normal((HIDDEN_DIM,), dtype=np.float32) * 0.05,
        "w1": rng.standard_normal((HIDDEN_DIM, HIDDEN_DIM), dtype=np.float32) * 0.2,
        "b1": rng.standard_normal((HIDDEN_DIM,), dtype=np.float32) * 0.05,
        "w2": rng.standard_normal((HIDDEN_DIM, HIDDEN_DIM), dtype=np.float32) * 0.2,
        "b2": rng.standard_normal((HIDDEN_DIM,), dtype=np.float32) * 0.05,
        "w3": rng.standard_normal((OUTPUT_DIM, HIDDEN_DIM), dtype=np.float32) * 0.2,
        "b3": rng.standard_normal((OUTPUT_DIM,), dtype=np.float32) * 0.05,
    }


def full_forward(model: dict[str, np.ndarray], x: np.ndarray) -> np.ndarray:
    h0 = np.maximum(x @ model["w0"].T + model["b0"], 0.0)
    h1 = np.maximum(h0 @ model["w1"].T + model["b1"], 0.0)
    h2 = np.maximum(h1 @ model["w2"].T + model["b2"], 0.0)
    return h2 @ model["w3"].T + model["b3"]


def _export_onnx(module, sample, path: Path, *, input_name: str, output_name: str) -> None:
    import torch

    module.eval()
    torch.onnx.export(
        module,
        sample,
        str(path),
        input_names=[input_name],
        output_names=[output_name],
        opset_version=17,
        dynamic_axes=None,
    )


def split_model(output_dir: str | Path) -> dict:
    import torch

    torch.set_num_threads(1)

    class FullFcOnnx(torch.nn.Module):
        def __init__(self, model: dict[str, np.ndarray]):
            super().__init__()
            self.register_buffer("w0", torch.from_numpy(model["w0"]).float())
            self.register_buffer("b0", torch.from_numpy(model["b0"]).float())
            self.register_buffer("w1", torch.from_numpy(model["w1"]).float())
            self.register_buffer("b1", torch.from_numpy(model["b1"]).float())
            self.register_buffer("w2", torch.from_numpy(model["w2"]).float())
            self.register_buffer("b2", torch.from_numpy(model["b2"]).float())
            self.register_buffer("w3", torch.from_numpy(model["w3"]).float())
            self.register_buffer("b3", torch.from_numpy(model["b3"]).float())

        def forward(self, x):
            h0 = torch.relu(x @ self.w0.T + self.b0)
            h1 = torch.relu(h0 @ self.w1.T + self.b1)
            h2 = torch.relu(h1 @ self.w2.T + self.b2)
            return h2 @ self.w3.T + self.b3

    class FcShardOnnx(torch.nn.Module):
        def __init__(self, weight: np.ndarray, bias: np.ndarray, *, activation: bool):
            super().__init__()
            self.register_buffer("weight", torch.from_numpy(weight).float())
            self.register_buffer("bias", torch.from_numpy(bias).float())
            self.activation = activation

        def forward(self, x):
            y = x @ self.weight.T + self.bias
            return torch.relu(y) if self.activation else y

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    model = make_full_model()
    sample_x = torch.from_numpy(make_input())
    full_path = output / "fc-full.onnx"
    _export_onnx(FullFcOnnx(model), sample_x, full_path,
                 input_name="x", output_name="output")
    summary = analyze_onnx_graph(full_path)
    candidates = estimate_split_candidates(summary, max_candidates=20)
    recommendations = recommend_sequential_splits(
        [
            SequentialSplitCandidate.from_onnx_candidate(candidate)
            for candidate in candidates
        ],
        providers=homogeneous_provider_profiles(
            [
                PROVIDER_PREFIX,
                PROVIDER_PREFIX + "/A",
            ]
        ),
        total_nodes=max(1, len(summary.nodes)),
    )
    graph_summary = output / "fc-full-onnx-graph-summary.json"
    write_onnx_graph_summary(
        graph_summary,
        full_model_summary=summary,
        split_candidates=candidates,
        planner_recommendations=recommendations,
    )

    shard_specs = {
        ROLE_S0_0: (
            FcShardOnnx(model["w0"], model["b0"], activation=True),
            sample_x,
            "x",
            "h0",
            {"start": 0, "end": 1, "stage": 0, "shard": 0},
        ),
        ROLE_S0_1: (
            FcShardOnnx(model["w1"], model["b1"], activation=True),
            torch.randn(1, HIDDEN_DIM),
            "h0",
            "h1",
            {"start": 1, "end": 2, "stage": 0, "shard": 1},
        ),
        ROLE_S1_0: (
            FcShardOnnx(model["w2"], model["b2"], activation=True),
            torch.randn(1, HIDDEN_DIM),
            "h1",
            "h2",
            {"start": 2, "end": 3, "stage": 1, "shard": 0},
        ),
        ROLE_S1_1: (
            FcShardOnnx(model["w3"], model["b3"], activation=False),
            torch.randn(1, HIDDEN_DIM),
            "h2",
            "output",
            {"start": 3, "end": 4, "stage": 1, "shard": 1, "final": True},
        ),
    }
    paths = {}
    metadata = {}
    for role, (module, sample, input_name, output_name, meta) in shard_specs.items():
        name = role.strip("/").replace("/", "-") + ".onnx"
        path = output / name
        _export_onnx(module, sample, path,
                     input_name=input_name, output_name=output_name)
        paths[role] = path
        metadata[role] = meta
    dependencies, chunk_graph = _build_fc_onnx_dependencies(paths)
    return {
        "paths": paths,
        "model": model,
        "full_onnx_model": full_path,
        "onnx_graph_summary": graph_summary,
        "onnx_split_candidates": candidates,
        "planner_recommendations": recommendations,
        "metadata": metadata,
        "dependencies": dependencies,
        "chunk_graph": chunk_graph,
    }


def _build_fc_onnx_dependencies(paths: dict[str, Path]) -> tuple[list[InferenceDependency], dict]:
    return build_sequential_chunk_dependencies([
        OnnxChunkSpec(
            role=ROLE_S0_0,
            path=str(paths[ROLE_S0_0]),
            key_scope="stage0-internal",
        ),
        OnnxChunkSpec(
            role=ROLE_S0_1,
            path=str(paths[ROLE_S0_1]),
            key_scope="stage0-to-stage1",
        ),
        OnnxChunkSpec(
            role=ROLE_S1_0,
            path=str(paths[ROLE_S1_0]),
            key_scope="stage1-internal",
        ),
        OnnxChunkSpec(
            role=ROLE_S1_1,
            path=str(paths[ROLE_S1_1]),
        ),
    ])


def pytorch_splitter_output(split: dict) -> SplitterOutput:
    artifacts = []
    for role, path in split["paths"].items():
        artifacts.append(SplitArtifact(
            role=role,
            path=str(path),
            artifact_name="/Model/PyTorch/FcOnnx2x2" + role,
            kind="onnx-model",
            backend="onnxruntime",
            metadata=dict(split.get("metadata", {}).get(role, {})),
        ))
    service = SplitServiceSpec(
        name=SERVICE,
        model_name="/Model/PyTorch/FcOnnx2x2",
        roles=list(ROLES),
        dependencies=list(split.get("dependencies") or []),
        artifacts=artifacts,
        input_schema={
            "codec": "npz",
            "encoder": "encode_input(input_array)",
            "fields": {
                "x": {
                    "dtype": "float32",
                    "shape": [1, INPUT_DIM],
                },
            },
        },
        output_schema={
            "codec": "npz",
            "decoder": "decode_output(response.payload)",
            "fields": {
                "output": {
                    "dtype": "float32",
                    "shape": [1, OUTPUT_DIM],
                },
            },
        },
        metadata={
            "source_model": "PyTorch FullFcOnnx",
            "splitter": "fc-onnx-2x2",
            "full_onnx_model": str(split.get("full_onnx_model", "")),
            "onnx_graph_summary": str(split.get("onnx_graph_summary", "")),
            "onnx_split_candidate_count": len(split.get("onnx_split_candidates") or []),
            "planner_recommendation_count": len(split.get("planner_recommendations") or []),
            "dependency_source": "onnx-chunk-io",
        },
    )
    return SplitterOutput(
        application="pytorch-fc-onnx-2x2-demo",
        controller=CONTROLLER,
        group=GROUP,
        user=USER,
        provider_prefix=PROVIDER_PREFIX,
        services=[service],
        trust_app_roots=["/NDNSF-DistributeInference/example"],
    )


def encode_input(x: np.ndarray) -> bytes:
    return tensor_payload({"x": np.asarray(x, dtype=np.float32)})


def decode_input(payload: bytes) -> np.ndarray:
    return np.asarray(load_tensor_payload(payload)["x"], dtype=np.float32)


def encode_hidden(offset: int, value: np.ndarray) -> bytes:
    return tensor_payload({
        "offset": np.asarray([int(offset)], dtype=np.int64),
        "hidden": np.asarray(value, dtype=np.float32),
    })


def decode_hidden(payload: bytes) -> tuple[int, np.ndarray]:
    obj = load_tensor_payload(payload)
    return int(np.asarray(obj["offset"]).reshape(-1)[0]), np.asarray(obj["hidden"], dtype=np.float32)


def encode_output(offset: int, value: np.ndarray) -> bytes:
    return tensor_payload({
        "offset": np.asarray([int(offset)], dtype=np.int64),
        "output": np.asarray(value, dtype=np.float32),
    })


def decode_output(payload: bytes) -> tuple[int, np.ndarray]:
    obj = load_tensor_payload(payload)
    return int(np.asarray(obj["offset"]).reshape(-1)[0]), np.asarray(obj["output"], dtype=np.float32)


def run_onnx(path: str | Path, values: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    feed = {
        input_info.name: np.asarray(values[input_info.name], dtype=np.float32)
        for input_info in session.get_inputs()
    }
    outputs = session.run(None, feed)
    return {
        output.name: np.asarray(value, dtype=np.float32)
        for output, value in zip(session.get_outputs(), outputs)
    }


def parse_args_with_common(description: str):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-pytorch-2x2-policy")
    parser.add_argument("--group", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true")
    return parser
