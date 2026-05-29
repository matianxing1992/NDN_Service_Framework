"""YOLO-style 2x2 distributed-inference example helpers."""

from __future__ import annotations

import argparse
import gzip
import io
import json
import shutil
import subprocess
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

import numpy as np

from ndnsf_distributed_inference import InferenceDependency
from ndnsf_distributed_inference.plan import ArtifactSpec, RuntimeSpec
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)


SERVICE = "/AI/YOLO/2x2Inference"
GROUP = "/example/hello/group"
CONTROLLER = "/example/hello/controller"
USER = "/example/hello/user"
PROVIDER_PREFIX = "/example/hello/provider"
CONFIG_FILE = str(Path(__file__).with_name("yolo_policy.yaml"))
RUNTIME_NAME = "/Runtime/NDNSF/YOLO2x2/NumpyRunner/v1"
REPO_SERVICE = "/NDNSF/DistributedRepo"
REPO_PROVIDER = PROVIDER_PREFIX + "/D"

ROLE_S0_0 = "/Stage/0/Shard/0"
ROLE_S0_1 = "/Stage/0/Shard/1"
ROLE_S1_0 = "/Stage/1/Shard/0"
ROLE_S1_1 = "/Stage/1/Shard/1"
ROLES = [ROLE_S0_0, ROLE_S0_1, ROLE_S1_0, ROLE_S1_1]

IMAGE_SHAPE = (1, 3, 16, 16)
INPUT_DIM = int(np.prod(IMAGE_SHAPE))
HIDDEN_DIM = 8
OUTPUT_DIM = 6


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
    np.savez_compressed(buffer, **values)
    return gzip.compress(buffer.getvalue(), compresslevel=6)


def _load_npz_payload(payload: bytes) -> dict[str, np.ndarray]:
    data = gzip.decompress(payload)
    with np.load(io.BytesIO(data), allow_pickle=False) as npz:
        return {key: npz[key] for key in npz.files}


def make_input() -> np.ndarray:
    rng = np.random.default_rng(20260528)
    return rng.normal(0.0, 0.25, size=IMAGE_SHAPE).astype(np.float32)


def make_full_model() -> dict[str, np.ndarray]:
    rng = np.random.default_rng(20260528)
    return {
        "w0": rng.normal(0.0, 0.03, size=(HIDDEN_DIM, INPUT_DIM)).astype(np.float32),
        "b0": rng.normal(0.0, 0.01, size=(HIDDEN_DIM,)).astype(np.float32),
        "w1": rng.normal(0.0, 0.05, size=(OUTPUT_DIM, HIDDEN_DIM)).astype(np.float32),
        "b1": rng.normal(0.0, 0.01, size=(OUTPUT_DIM,)).astype(np.float32),
    }


def full_forward(model: dict[str, np.ndarray], image: np.ndarray) -> np.ndarray:
    x = image.astype(np.float32).reshape(1, INPUT_DIM)
    hidden = np.maximum(x @ model["w0"].T + model["b0"], 0.0)
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
            "hidden_offset": np.array(0, dtype=np.int64),
        },
        ROLE_S0_1: {
            "w0": model["w0"][hmid:],
            "b0": model["b0"][hmid:],
            "hidden_offset": np.array(hmid, dtype=np.int64),
        },
        ROLE_S1_0: {
            "w1": model["w1"][:omid],
            "b1": model["b1"][:omid],
            "output_offset": np.array(0, dtype=np.int64),
        },
        ROLE_S1_1: {
            "w1": model["w1"][omid:],
            "b1": model["b1"][omid:],
            "output_offset": np.array(omid, dtype=np.int64),
        },
    }
    paths = {}
    for role, arrays in parts.items():
        name = role.strip("/").replace("/", "-") + ".npz.gz"
        path = output / name
        path.write_bytes(_npz_payload(arrays))
        paths[role] = path
    return {"paths": paths, "model": model}


def yolo_splitter_output(split: dict) -> SplitterOutput:
    artifacts = []
    for role, path in split["paths"].items():
        artifacts.append(SplitArtifact(
            role=role,
            path=str(path),
            artifact_name="/Model/Ultralytics/YOLO/2x2" + role,
            kind="numpy-yolo-shard",
            backend="numpy",
        ))
    service = SplitServiceSpec(
        name=SERVICE,
        model_name="/Model/Ultralytics/YOLO/2x2",
        roles=list(ROLES),
        dependencies=[
            InferenceDependency(
                producers=[ROLE_S0_0, ROLE_S0_1],
                consumers=[ROLE_S1_0, ROLE_S1_1],
                key_scope="stage0-to-stage1",
                topic_prefix="/activation",
            ),
            InferenceDependency(
                producers=[ROLE_S1_0, ROLE_S1_1],
                consumers=[ROLE_S1_0, ROLE_S1_1],
                key_scope="stage1-internal",
                topic_prefix="/detections",
            ),
        ],
        artifacts=artifacts,
        input_schema={
            "codec": "npz+gzip",
            "encoder": "encode_image_for_yolo(image_tensor)",
            "fields": {
                "images": {
                    "dtype": "float32",
                    "shape": list(IMAGE_SHAPE),
                    "layout": "NCHW",
                },
            },
        },
        output_schema={
            "codec": "npz+gzip",
            "decoder": "decode_yolo_output(response.payload)",
            "fields": {
                "output": {
                    "dtype": "float32",
                    "shape": [1, OUTPUT_DIM],
                },
            },
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
        application="yolo-2x2-demo",
        controller=CONTROLLER,
        group=GROUP,
        user=USER,
        provider_prefix=PROVIDER_PREFIX,
        services=[service, repo_service],
        trust_app_roots=["/example"],
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
        trust_app_roots=output.trust_app_roots,
        trust_anchor_file=trust_anchor_file,
        artifact_allowlist=[RUNTIME_NAME],
        artifact_sandbox={
            "kind": "local-python",
            "command": ["python3"],
        },
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
        backend="numpy",
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
    service_policy = client.deployment.service_policy(SERVICE)
    runtime = runtime_spec()
    builder = client.plan_builder(SERVICE, runtime=runtime, backend="numpy")
    artifacts = {artifact.role: artifact for artifact in service_policy.artifacts}
    for role in ROLES:
        artifact = artifacts[role]
        builder.add_part(
            role=role,
            model=artifact.path,
            artifact_name=artifact.artifact_name,
            filename=artifact.filename,
            kind=artifact.kind,
            backend=artifact.backend or "numpy",
            runtime=runtime,
            cache_name=artifact.artifact_name,
            allow_dynamic_provisioning=True,
        )
    return builder.build()


def load_repo_manifests(path: str | Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def build_repo_plan(client, manifest_path: str | Path):
    manifests = load_repo_manifests(manifest_path)
    service_policy = client.deployment.service_policy(SERVICE)
    runtime = runtime_spec()
    builder = client.plan_builder(SERVICE, runtime=runtime, backend="numpy")
    artifacts = {artifact.role: artifact for artifact in service_policy.artifacts}
    for role in ROLES:
        artifact = artifacts[role]
        role_manifests = manifests["roles"][role]
        builder.add_part(
            role=role,
            model=b"",
            artifact_name=artifact.artifact_name,
            filename=artifact.filename,
            kind=artifact.kind,
            backend=artifact.backend or "numpy",
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
                    repo_manifest=role_manifests["runner"],
                ),
            ),
            cache_name=artifact.artifact_name,
            repo_manifest=role_manifests["model"],
            allow_dynamic_provisioning=True,
        )
    return builder.build()


def encode_image_for_yolo(image: np.ndarray) -> bytes:
    return _npz_payload({"images": image.astype(np.float32)})


def decode_image(payload: bytes) -> np.ndarray:
    return _load_npz_payload(payload)["images"].astype(np.float32)


def encode_hidden(offset: int, value: np.ndarray) -> bytes:
    return _npz_payload({
        "offset": np.array(offset, dtype=np.int64),
        "hidden": value.astype(np.float32),
    })


def decode_hidden(payload: bytes) -> tuple[int, np.ndarray]:
    obj = _load_npz_payload(payload)
    return int(obj["offset"]), obj["hidden"].astype(np.float32)


def encode_yolo_output(offset: int, value: np.ndarray) -> bytes:
    return _npz_payload({
        "offset": np.array(offset, dtype=np.int64),
        "output": value.astype(np.float32),
    })


def decode_yolo_output(payload: bytes) -> tuple[int, np.ndarray]:
    obj = _load_npz_payload(payload)
    return int(obj["offset"]), obj["output"].astype(np.float32)


def load_part(path: str | Path) -> dict[str, np.ndarray]:
    return _load_npz_payload(Path(path).read_bytes())


def concat_by_offset(items: list[tuple[int, np.ndarray]]) -> np.ndarray:
    return np.concatenate([value for _, value in sorted(items, key=lambda item: item[0])],
                          axis=1)


def parse_args_with_common(description: str):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--group", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true")
    return parser
