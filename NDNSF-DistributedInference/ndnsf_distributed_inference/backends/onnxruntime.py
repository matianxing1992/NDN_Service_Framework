"""ONNX Runtime helper backend for NDNSF-DistributedInference."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from ..plan import ArtifactSpec, RuntimeSpec


RUNTIME_NAME = "/Runtime/ONNXRuntime/CPU"


def build_runner_script() -> bytes:
    """Return a generic executable ONNX Runtime runner artifact."""

    return b'''#!/usr/bin/env python3
import sys

import numpy as np
import onnxruntime as ort


def load_npz(path):
    with np.load(path) as data:
        return {name: data[name].astype(np.float32) for name in data.files}


def save_npz(path, values):
    with open(path, "wb") as f:
        np.savez_compressed(f, **values)


def main():
    if len(sys.argv) != 5:
        print("usage: onnxruntime_runner.py <stage> <model.onnx> <input.npz> <output.npz>",
              file=sys.stderr)
        return 2
    stage, model_path, input_path, output_path = sys.argv[1:]
    session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    values = load_npz(input_path)
    feed = {input_info.name: values[input_info.name]
            for input_info in session.get_inputs()}
    outputs = session.run(None, feed)
    if stage == "stage0":
        save_npz(output_path, {
            output_info.name: np.asarray(value, dtype=np.float32)
            for output_info, value in zip(session.get_outputs(), outputs)
        })
        return 0
    if stage == "stage1":
        save_npz(output_path, {"output": np.asarray(outputs[0], dtype=np.float32)})
        return 0
    print(f"unsupported stage {stage}", file=sys.stderr)
    return 2


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
            filename="onnxruntime_runner.py",
            kind="runtime-script",
            executable=False,
            cache_name="/NDNSF/Runtime/ONNXRuntime/CPU/PythonRunner/v1",
        ),
    )


def run_downloaded_runner(
    execution,
    stage: str,
    input_payload: bytes,
    *,
    timeout_s: float = 60.0,
) -> bytes:
    model_path = execution.path("model")
    runner_path = execution.path("runner")
    input_path = Path(execution.work_dir) / f"{stage}-input.npz"
    output_path = Path(execution.work_dir) / f"{stage}-output.npz"
    input_path.write_bytes(input_payload)
    subprocess.run(
        [sys.executable, str(runner_path), stage, str(model_path),
         str(input_path), str(output_path)],
        check=True,
        cwd=str(execution.work_dir),
        timeout=timeout_s,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return output_path.read_bytes()
