"""Pure compatibility evaluation shared by container probes and adapters."""

from __future__ import annotations

import re
from typing import Any, Mapping


REQUIRED_BINARIES = ("nfd", "nfdc", "App_ServiceController", "di-native-provider")
REQUIRED_IMPORTS = ("ndnsf", "ndnsf_distributed_inference", "torch", "transformers", "onnxruntime")
MINIMUM_DRIVER = (550, 54, 14)


class GpuCompatibilityError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise GpuCompatibilityError(code + (f":{detail}" if detail else ""))


def _version(value: object, field: str) -> tuple[int, ...]:
    if not isinstance(value, str) or re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", value) is None:
        _fail("FAIL_VERSION_INVALID", field)
    return tuple(int(part) for part in value.split("."))


def evaluate_runtime_facts(value: Mapping[str, object]) -> dict[str, Any]:
    required = {
        "binaries", "imports", "missingLibraries", "driverVersion", "torchCudaAvailable",
        "torchCudaVersion", "torchCudnnMajor", "ortProviders", "ortCudaVersion",
        "ortCudnnMajor", "profileProviders", "allocatedGpuUuid", "torchGpuUuid", "ortGpuUuid",
    }
    if not isinstance(value, Mapping) or set(value) != required:
        _fail("FAIL_RUNTIME_FACT_FIELDS")
    binaries = value["binaries"]
    imports = value["imports"]
    if not isinstance(binaries, Mapping) or any(not binaries.get(name) for name in REQUIRED_BINARIES):
        _fail("FAIL_RUNTIME_BINARY_MISSING")
    if not isinstance(imports, Mapping) or any(not imports.get(name) for name in REQUIRED_IMPORTS):
        _fail("FAIL_RUNTIME_IMPORT_MISSING")
    if value["missingLibraries"]:
        _fail("FAIL_RUNTIME_LIBRARY_MISSING")
    if _version(value["driverVersion"], "driverVersion") < MINIMUM_DRIVER:
        _fail("FAIL_DRIVER_TOO_OLD")
    if value["torchCudaAvailable"] is not True:
        _fail("FAIL_PYTORCH_CUDA_UNAVAILABLE")
    torch_cuda = _version(value["torchCudaVersion"], "torchCudaVersion")
    ort_cuda = _version(value["ortCudaVersion"], "ortCudaVersion")
    if torch_cuda[0] != ort_cuda[0]:
        _fail("FAIL_PYTORCH_ORT_CUDA_MISMATCH")
    if torch_cuda[0] != 12 or ort_cuda[0] != 12:
        _fail("FAIL_CUDA_MAJOR_MISMATCH")
    if value["torchCudnnMajor"] != 9 or value["ortCudnnMajor"] != 9:
        _fail("FAIL_CUDNN_MAJOR_MISMATCH")
    if "CUDAExecutionProvider" not in value["ortProviders"]:
        _fail("FAIL_ORT_CUDA_PROVIDER_MISSING")
    providers = value["profileProviders"]
    if not isinstance(providers, list) or not providers or any(item != "CUDAExecutionProvider" for item in providers):
        _fail("FAIL_ORT_CPU_FALLBACK")
    allocated = value["allocatedGpuUuid"]
    if not allocated or value["torchGpuUuid"] != allocated or value["ortGpuUuid"] != allocated:
        _fail("FAIL_GPU_UUID_MISMATCH")
    return {
        "status": "PASS", "driverVersion": value["driverVersion"],
        "cudaMajor": 12, "cudnnMajor": 9, "cpuFallback": False,
        "allocatedGpuUuid": allocated,
    }


__all__ = ["GpuCompatibilityError", "evaluate_runtime_facts"]
