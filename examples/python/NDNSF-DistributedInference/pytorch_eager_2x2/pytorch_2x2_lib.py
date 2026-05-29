"""PyTorch eager 2x2 distributed-inference example helpers."""

from __future__ import annotations

import argparse
import gzip
import io
import shutil
import subprocess
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

import torch

from ndnsf_distributed_inference import InferenceDependency
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)


SERVICE = "/AI/PyTorch/Eager2x2Inference"
GROUP = "/example/hello/group"
CONTROLLER = "/example/hello/controller"
USER = "/example/hello/user"
PROVIDER_PREFIX = "/example/hello/provider"
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


def tensor_payload(value) -> bytes:
    buffer = io.BytesIO()
    torch.save(value, buffer)
    return gzip.compress(buffer.getvalue(), compresslevel=6)


def load_tensor_payload(payload: bytes):
    data = gzip.decompress(payload)
    kwargs = {"map_location": "cpu"}
    try:
        kwargs["weights_only"] = True
        return torch.load(io.BytesIO(data), **kwargs)
    except TypeError:
        kwargs.pop("weights_only", None)
        return torch.load(io.BytesIO(data), **kwargs)


def make_input() -> torch.Tensor:
    torch.manual_seed(20260528)
    return torch.randn(1, INPUT_DIM)


def make_full_model() -> dict[str, torch.Tensor]:
    torch.manual_seed(20260528)
    return {
        "w0": torch.randn(HIDDEN_DIM, INPUT_DIM) * 0.2,
        "b0": torch.randn(HIDDEN_DIM) * 0.05,
        "w1": torch.randn(OUTPUT_DIM, HIDDEN_DIM) * 0.2,
        "b1": torch.randn(OUTPUT_DIM) * 0.05,
    }


def full_forward(model: dict[str, torch.Tensor], x: torch.Tensor) -> torch.Tensor:
    hidden = torch.relu(x @ model["w0"].T + model["b0"])
    return hidden @ model["w1"].T + model["b1"]


def split_model(output_dir: str | Path) -> dict:
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    model = make_full_model()
    hmid = HIDDEN_DIM // 2
    omid = OUTPUT_DIM // 2
    parts = {
        ROLE_S0_0: {
            "w0": model["w0"][:hmid],
            "b0": model["b0"][:hmid],
            "hidden_offset": 0,
        },
        ROLE_S0_1: {
            "w0": model["w0"][hmid:],
            "b0": model["b0"][hmid:],
            "hidden_offset": hmid,
        },
        ROLE_S1_0: {
            "w1": model["w1"][:omid],
            "b1": model["b1"][:omid],
            "output_offset": 0,
        },
        ROLE_S1_1: {
            "w1": model["w1"][omid:],
            "b1": model["b1"][omid:],
            "output_offset": omid,
        },
    }
    paths = {}
    for role, payload in parts.items():
        name = role.strip("/").replace("/", "-") + ".pt"
        path = output / name
        path.write_bytes(tensor_payload(payload))
        paths[role] = path
    return {"paths": paths, "model": model}


def pytorch_splitter_output(split: dict) -> SplitterOutput:
    artifacts = []
    for role, path in split["paths"].items():
        artifacts.append(SplitArtifact(
            role=role,
            path=str(path),
            artifact_name="/Model/PyTorch/Eager2x2" + role,
            kind="pytorch-eager-state",
            backend="pytorch-eager",
        ))
    service = SplitServiceSpec(
        name=SERVICE,
        model_name="/Model/PyTorch/Eager2x2",
        roles=list(ROLES),
        dependencies=[
            InferenceDependency(
                producers=[ROLE_S0_0, ROLE_S0_1],
                consumers=[ROLE_S1_0, ROLE_S1_1],
                key_scope="stage0-to-stage1",
                topic_prefix="/hidden",
            ),
            InferenceDependency(
                producers=[ROLE_S1_0, ROLE_S1_1],
                consumers=[ROLE_S1_0, ROLE_S1_1],
                key_scope="stage1-internal",
                topic_prefix="/output",
            ),
        ],
        artifacts=artifacts,
        input_schema={
            "codec": "torch-save+gzip",
            "encoder": "encode_input(input_tensor)",
            "fields": {
                "x": {
                    "dtype": "float32",
                    "shape": [1, INPUT_DIM],
                },
            },
        },
        output_schema={
            "codec": "torch-save+gzip",
            "decoder": "decode_output(response.payload)",
            "fields": {
                "output": {
                    "dtype": "float32",
                    "shape": [1, OUTPUT_DIM],
                },
            },
        },
    )
    return SplitterOutput(
        application="pytorch-eager-2x2-demo",
        controller=CONTROLLER,
        group=GROUP,
        user=USER,
        provider_prefix=PROVIDER_PREFIX,
        services=[service],
        trust_app_roots=["/example"],
    )


def encode_input(x: torch.Tensor) -> bytes:
    return tensor_payload({"x": x.float().cpu()})


def decode_input(payload: bytes) -> torch.Tensor:
    return load_tensor_payload(payload)["x"].float()


def encode_hidden(offset: int, value: torch.Tensor) -> bytes:
    return tensor_payload({"offset": int(offset), "hidden": value.float().cpu()})


def decode_hidden(payload: bytes) -> tuple[int, torch.Tensor]:
    obj = load_tensor_payload(payload)
    return int(obj["offset"]), obj["hidden"].float()


def encode_output(offset: int, value: torch.Tensor) -> bytes:
    return tensor_payload({"offset": int(offset), "output": value.float().cpu()})


def decode_output(payload: bytes) -> tuple[int, torch.Tensor]:
    obj = load_tensor_payload(payload)
    return int(obj["offset"]), obj["output"].float()


def load_part(path: str | Path) -> dict:
    return load_tensor_payload(Path(path).read_bytes())


def parse_args_with_common(description: str):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-pytorch-2x2-policy")
    parser.add_argument("--group", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true")
    return parser
