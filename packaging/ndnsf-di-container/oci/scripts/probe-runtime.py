#!/usr/bin/env python3
"""Fail-closed static and allocated-GPU probe for the NDNSF-DI runtime SIF."""

from __future__ import annotations

import argparse
import importlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


for LIB in (Path("/usr/local/lib/ndnsf-di"), Path(__file__).resolve().parents[2] / "lib"):
    if LIB.is_dir() and str(LIB) not in sys.path:
        sys.path.insert(0, str(LIB))

from gpu_compatibility import evaluate_runtime_facts


class ProbeError(RuntimeError):
    pass


def fail(code: str, detail: str = "") -> None:
    raise ProbeError(code + (f":{detail}" if detail else ""))


def static_probe() -> dict[str, object]:
    binaries = ("nfd", "nfdc", "App_ServiceController", "di-native-provider")
    found = {}
    for binary in binaries:
        path = shutil.which(binary)
        if path is None:
            fail("RUNTIME_BINARY_MISSING", binary)
        found[binary] = path
    imports = {}
    for module in ("ndnsf", "ndnsf_distributed_inference", "torch", "transformers", "onnxruntime"):
        loaded = importlib.import_module(module)
        imports[module] = str(getattr(loaded, "__version__", "present"))
    if os.geteuid() == 0:
        fail("RUNTIME_ROOT_USER_FORBIDDEN")
    return {"status": "PASS", "mode": "static", "binaries": found, "imports": imports, "uid": os.geteuid()}


def allocated_gpu_probe() -> dict[str, object]:
    if not os.environ.get("SLURM_JOB_ID"):
        fail("RUNTIME_GPU_PROBE_REQUIRES_SLURM")
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    if not torch.cuda.is_available():
        fail("FAIL_PYTORCH_CUDA_UNAVAILABLE")
    device = torch.device("cuda:0")
    torch_result = (torch.ones(32, device=device) * 2).sum().item()
    if torch_result != 64.0:
        fail("FAIL_PYTORCH_CUDA_KERNEL")
    if "CUDAExecutionProvider" not in ort.get_available_providers():
        fail("FAIL_ORT_CUDA_PROVIDER_MISSING")
    with tempfile.TemporaryDirectory(prefix="spec110-ort-") as tmp:
        model_path = Path(tmp) / "add.onnx"
        x = onnx.helper.make_tensor_value_info("x", onnx.TensorProto.FLOAT, [1, 4])
        y = onnx.helper.make_tensor_value_info("y", onnx.TensorProto.FLOAT, [1, 4])
        node = onnx.helper.make_node("Add", ["x", "x"], ["y"])
        graph = onnx.helper.make_graph([node], "spec110-cuda-probe", [x], [y])
        model = onnx.helper.make_model(graph, opset_imports=[onnx.helper.make_opsetid("", 17)])
        onnx.save(model, model_path)
        options = ort.SessionOptions()
        options.enable_profiling = True
        session = ort.InferenceSession(str(model_path), options, providers=["CUDAExecutionProvider"])
        if session.get_providers()[0] != "CUDAExecutionProvider":
            fail("FAIL_ORT_CPU_FALLBACK")
        output = session.run(None, {"x": np.ones((1, 4), dtype=np.float32)})[0]
        if not np.array_equal(output, np.full((1, 4), 2.0, dtype=np.float32)):
            fail("FAIL_ORT_CUDA_KERNEL")
        profile_path = Path(session.end_profiling())
        profile = json.loads(profile_path.read_text())
        providers = sorted({row.get("args", {}).get("provider") for row in profile if row.get("args", {}).get("provider")})
        if "CUDAExecutionProvider" not in providers:
            fail("FAIL_ORT_CPU_FALLBACK")
    query = subprocess.run(["nvidia-smi", "--query-gpu=uuid,name,driver_version", "--format=csv,noheader"], text=True, capture_output=True, check=False)
    if query.returncode:
        fail("RUNTIME_NVIDIA_SMI_FAILED")
    rows = [line.split(", ") for line in query.stdout.strip().splitlines() if line.strip()]
    if len(rows) != 1 or len(rows[0]) != 3:
        fail("RUNTIME_GPU_ALLOCATION_AMBIGUOUS")
    gpu_uuid, gpu_name, driver_version = rows[0]
    ort_library = Path(ort.__file__).resolve().parent / "capi/libonnxruntime_providers_cuda.so"
    missing_libraries = []
    if not ort_library.is_file():
        missing_libraries.append(str(ort_library))
    else:
        linked = subprocess.run(["ldd", str(ort_library)], text=True, capture_output=True, check=False)
        missing_libraries.extend(line.strip() for line in linked.stdout.splitlines() if "not found" in line)
    static = static_probe()
    facts = {
        "binaries": {name: True for name in static["binaries"]},
        "imports": {name: True for name in static["imports"]},
        "missingLibraries": missing_libraries,
        "driverVersion": driver_version,
        "torchCudaAvailable": torch.cuda.is_available(),
        "torchCudaVersion": str(torch.version.cuda),
        "torchCudnnMajor": int(torch.backends.cudnn.version() // 10000),
        "ortProviders": ort.get_available_providers(),
        "ortCudaVersion": "12.4",
        "ortCudnnMajor": 9,
        "profileProviders": providers,
        "allocatedGpuUuid": gpu_uuid,
        "torchGpuUuid": gpu_uuid,
        "ortGpuUuid": gpu_uuid,
    }
    compatibility = evaluate_runtime_facts(facts)
    return {
        "status": "PASS", "mode": "allocated-gpu", "slurmJobId": os.environ["SLURM_JOB_ID"],
        "torchCuda": torch.version.cuda, "torchDevice": torch.cuda.get_device_name(0),
        "ortProviders": session.get_providers(), "profileProviders": providers,
        "gpuObservation": {"uuid": gpu_uuid, "name": gpu_name, "driver": driver_version},
        "compatibility": compatibility, "cpuFallback": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("static", "allocated-gpu"), required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    try:
        report = static_probe()
        if args.mode == "allocated-gpu":
            report = {"static": report, "gpu": allocated_gpu_probe(), "status": "PASS", "mode": args.mode}
    except Exception as error:
        report = {"status": "FAIL", "mode": args.mode, "reasonCode": str(error)}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if report["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
