#!/usr/bin/env python3
"""Static, fail-closed preflight for the sealed iTiger GPU OCI build graph."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import tarfile


RELIC_REVISION = "b984e901ba78c83ea4093ea96addd13628c8c2d0"
WEBSOCKETPP_REVISION = "ac4e021333675fc80b96eb7be45d218581c897e2"
NDN_SVS_REVISION = "7b616b08624a79617bb05f2d3553bbbacdc4c482"
NAC_ABE_REVISION = "390e9001a8611e04c90f3a5866d09c3136c885d0"
FOUNDATION_BASE = "ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214"
PYTHON_BASE = "python@sha256:b3061b93c8df9809c3783a4f17bbf2520425ec6b40bd3e5e7538870e21ba7209"
GPU_BUILD_BASE = "nvidia/cuda@sha256:f18cf1a9ac2842e59f13b0d0729594da8cbd68cadd2379308cdd98c0374dbd80"
GPU_RUNTIME_BASE = "nvidia/cuda@sha256:a6a8417cb56c9a5d30c4d8c78ad18bc9b75ffe4453fe1c04b3149b3741518b06"
REQUIRED_SYSTEM = {"bison", "flex", "libfl-dev", "libgtest-dev", "libpcap-dev"}
REQUIRED_PYTHON_RUNTIME = {
    "libgdbm6", "libreadline8", "libsqlite3-0", "libssl1.1",
}
REQUIRED_PYTHON = {
    "certifi", "charset-normalizer", "coloredlogs", "filelock", "flatbuffers",
    "fsspec", "humanfriendly", "idna", "jinja2", "markupsafe", "mpmath",
    "networkx", "nvidia-cusparselt-cu12", "nvidia-ml-py", "nvidia-nccl-cu12",
    "packaging", "pillow", "pyyaml", "regex", "requests", "sympy", "tqdm",
    "triton", "typing-extensions", "urllib3",
}
REQUIRED_QWEN_PYTHON = {"onnxruntime-gpu", "torch", "transformers"}
UNUSED_TORCH_MEDIA = {"torchaudio", "torchvision"}
REQUIRED_SYSTEM_PYTHON = {
    "nvidia-cublas-cu12", "nvidia-cuda-cupti-cu12", "nvidia-cuda-nvrtc-cu12",
    "nvidia-cuda-runtime-cu12", "nvidia-cudnn-cu12", "nvidia-cufft-cu12",
    "nvidia-curand-cu12", "nvidia-cusolver-cu12", "nvidia-cusparse-cu12",
    "nvidia-nvjitlink-cu12", "nvidia-nvtx-cu12",
}
ARCHIVE_MARKERS = {
    "ndn-cxx": "wscript",
    "ndn-svs": "wscript",
    "NDNSD": "wscript",
    "NFD": "wscript",
    "openabe": "Makefile",
    "relic": "CMakeLists.txt",
    "NAC-ABE": "CMakeLists.txt",
    "websocketpp": "websocketpp/version.hpp",
}


class PreflightError(RuntimeError):
    pass


def require(condition: bool, code: str) -> None:
    if not condition:
        raise PreflightError(code)


def validate_archives(seal_root: Path, sources: set[str]) -> int:
    seal = json.loads((seal_root / "source-seal.json").read_text())
    dependencies = seal.get("dependencies", {})
    require(set(dependencies) == sources, "PREFLIGHT_SEAL_SOURCE_SET_MISMATCH")
    checked = 0
    for name, marker in ARCHIVE_MARKERS.items():
        archive_path = seal_root / dependencies[name]["archivePath"]
        with tarfile.open(archive_path, "r:") as archive:
            names = set(archive.getnames())
        require(marker in names, f"PREFLIGHT_ARCHIVE_ENTRY_MISSING:{name}:{marker}")
        checked += 1
    return checked


def run(workspace: Path, seal_root: Path | None) -> dict[str, object]:
    oci = workspace / "packaging/ndnsf-di-container/oci"
    lock = json.loads((oci / "locks/gpu.lock").read_text())
    foundation = (oci / "Dockerfile.foundation").read_text()
    dockerfile = (oci / "Dockerfile.gpu").read_text()
    runtime_closure = (oci / "scripts/verify-runtime-closure.py").read_text()
    matrix = (oci / "compatibility/gpu-matrix.yaml").read_text()
    workflow = (workspace / ".github/workflows/ndnsf-di-itiger-image.yml").read_text()
    gitignore = (workspace / ".gitignore").read_text().splitlines()
    sources = lock.get("sourceRepositories", {})
    require(lock.get("schemaVersion") == "ndnsf-di-gpu-lock-v1", "PREFLIGHT_LOCK_SCHEMA_INVALID")
    require(".spec110-build/" in gitignore, "PREFLIGHT_TRANSIENT_SEAL_NOT_IGNORED")
    require(REQUIRED_SYSTEM <= set(lock.get("systemPackages", [])), "PREFLIGHT_SYSTEM_CLOSURE_INCOMPLETE")
    require(lock.get("distributionBase") == "ubuntu20.04-openssl1.1", "PREFLIGHT_OPENABE_ABI_BASE_INVALID")
    require(
        set(lock.get("pythonRuntimePackages", [])) == REQUIRED_PYTHON_RUNTIME,
        "PREFLIGHT_PYTHON_RUNTIME_ABI_CLOSURE_INVALID",
    )
    require(
        set(lock.get("pythonExcludedOptionalExtensions", [])) == {"nis", "_tkinter"},
        "PREFLIGHT_PYTHON_OPTIONAL_EXTENSION_POLICY_INVALID",
    )
    require(lock.get("baseImages") == {
        "foundation": FOUNDATION_BASE,
        "python": PYTHON_BASE,
        "gpuBuild": GPU_BUILD_BASE,
        "gpuRuntime": GPU_RUNTIME_BASE,
    }, "PREFLIGHT_BASE_IMAGE_CONTRACT_INVALID")
    require(sources.get("relic", {}).get("revision") == RELIC_REVISION, "PREFLIGHT_RELIC_NOT_LOCKED")
    require(sources.get("websocketpp", {}).get("revision") == WEBSOCKETPP_REVISION, "PREFLIGHT_WEBSOCKETPP_NOT_LOCKED")
    require(sources.get("ndn-svs", {}).get("revision") == NDN_SVS_REVISION, "PREFLIGHT_NDN_SVS_COMPATIBILITY_NOT_LOCKED")
    require(sources.get("NAC-ABE", {}).get("revision") == NAC_ABE_REVISION, "PREFLIGHT_NAC_ABE_TEST_LINK_NOT_LOCKED")
    relations = lock.get("sourceRelationships", {})
    require(relations.get("NFD.websocketpp") == WEBSOCKETPP_REVISION, "PREFLIGHT_NFD_GITLINK_MISMATCH")
    require(relations.get("openabe.relic") == RELIC_REVISION, "PREFLIGHT_OPENABE_RELIC_MISMATCH")
    onnx_cpp = lock.get("onnxRuntimeCpp", {})
    require(onnx_cpp.get("version") == "1.20.1", "PREFLIGHT_ONNX_CPP_VERSION_INVALID")
    require(onnx_cpp.get("sha256") == "6bfb87c6ebe55367a94509b8ef062239e188dccf8d5caac8d6909b2344893bf0", "PREFLIGHT_ONNX_CPP_DIGEST_INVALID")
    require(onnx_cpp.get("bytes") == 258487100, "PREFLIGHT_ONNX_CPP_SIZE_INVALID")
    require(
        lock.get("onnxRuntimeExcludedOptionalProviders") == ["TensorrtExecutionProvider"],
        "PREFLIGHT_ONNX_OPTIONAL_PROVIDER_POLICY_INVALID",
    )
    python = {name.lower() for name in lock.get("pythonPackages", {})}
    require(REQUIRED_PYTHON <= python, "PREFLIGHT_PYTHON_CLOSURE_INCOMPLETE")
    require(REQUIRED_QWEN_PYTHON <= python, "PREFLIGHT_QWEN_RUNTIME_INCOMPLETE")
    require(
        UNUSED_TORCH_MEDIA.isdisjoint(python),
        "PREFLIGHT_UNUSED_TORCH_MEDIA_PRESENT",
    )
    require(
        all(name not in dockerfile and f"{name}:" not in matrix for name in UNUSED_TORCH_MEDIA),
        "PREFLIGHT_UNUSED_TORCH_MEDIA_DECLARED",
    )
    system_python = {name.lower() for name in lock.get("pythonSystemProvidedPackages", {})}
    require(REQUIRED_SYSTEM_PYTHON == system_python, "PREFLIGHT_SYSTEM_CUDA_CONTRACT_INCOMPLETE")
    foundation_markers = (
        "ARG DEPENDENCY_SOURCE_MODE=sealed", ".spec110-build/archives",
        "SOURCE_SEAL_MANIFEST_TAMPERED", "cd /src/dependencies/ndn-cxx",
        "cd /src/dependencies/ndn-svs", "cd /src/dependencies/NDNSD",
        "cd /src/dependencies/NFD", "prepare-openabe-relic.py",
        'SHELL ["/bin/bash", "-o", "pipefail", "-c"]',
        "prepare-openabe-relic.py", "NO_DEPS=1", "make INSTALL_PREFIX=$PREFIX install",
        "-DHAVE_TESTS=TRUE", "NAC-ABE/build/tests/unit-tests -l test_suite -x",
        "derive-runtime-packages.py", "runtime-system-packages",
        "FROM scratch AS foundation",
    )
    for marker in foundation_markers:
        require(marker in foundation, f"PREFLIGHT_FOUNDATION_MARKER_MISSING:{marker}")
    require(foundation.index("cd /src/dependencies/ndn-cxx") < foundation.index("cd /src/dependencies/ndn-svs") < foundation.index("cd /src/dependencies/NDNSD"), "PREFLIGHT_FOUNDATION_BUILD_ORDER_INVALID")
    openabe_block = foundation.split("cd /src/dependencies/openabe", 1)[1].split("NAC-ABE", 1)[0]
    require("cmake -S . -B build" not in openabe_block, "PREFLIGHT_OPENABE_ADAPTER_INVALID")
    markers = (
        "ARG FOUNDATION_IMAGE", "FROM ${FOUNDATION_IMAGE} AS local-foundation",
        "ARG PYTHON_BASE_IMAGE", "FROM ${PYTHON_BASE_IMAGE} AS python-runtime",
        "python3.10 -m venv",
        "lib-dynload/nis.*.so", "lib-dynload/_tkinter.*.so", "ldconfig",
        "--root /usr/local/bin --root /usr/local/lib/python3.10",
        "ONNXRUNTIME_CPP_SHA256", "sha256sum -c -", "--with-examples",
        "install -m 0755 build/examples/di-native-provider",
        "derive-runtime-packages.py", "runtime-system-packages", "/etc/ndn/nfd.conf",
        "/run/nfd", "verify-runtime-closure.py", "verify-python-environment.py",
        "ARG FOUNDATION_SOURCE_REVISION",
        "onnxRuntimeExcludedOptionalProviders",
        "/opt/onnxruntime/lib/libonnxruntime_providers_tensorrt.so",
        "/opt/venv/lib/python3.10/site-packages/onnxruntime/capi/libonnxruntime_providers_tensorrt.so",
        "/opt/onnxruntime/lib/libonnxruntime_providers_cuda.so",
        "/opt/venv/lib/python3.10/site-packages/onnxruntime/capi/libonnxruntime_providers_cuda.so",
        'org.ndnsf.di.foundation.revision="${FOUNDATION_SOURCE_REVISION}"',
        "missing_packages", "dpkg-query -W -f='${db:Status-Abbrev}'",
        "grep -qx '.i '",
    )
    for marker in markers:
        require(marker in dockerfile, f"PREFLIGHT_DOCKER_MARKER_MISSING:{marker}")
    for marker in (
        'HOST_DRIVER_LIBRARIES = {"libcuda.so.1"}',
        "path.parent.resolve()",
        "unresolved - HOST_DRIVER_LIBRARIES",
    ):
        require(
            marker in runtime_closure,
            f"PREFLIGHT_RUNTIME_CLOSURE_MARKER_MISSING:{marker}",
        )
    foundation_runtime_install = "$(cat /opt/ndnsf-di/manifest/runtime-system-packages)"
    closure_derivation = "python3 /build-contract/derive-runtime-packages.py"
    require(
        foundation_runtime_install in dockerfile,
        "PREFLIGHT_GPU_ASSEMBLER_FOUNDATION_RUNTIME_MISSING",
    )
    require(
        dockerfile.index(foundation_runtime_install) < dockerfile.index(closure_derivation),
        "PREFLIGHT_GPU_ASSEMBLER_RUNTIME_INSTALL_LATE",
    )
    require(".spec110-build" not in dockerfile, "PREFLIGHT_GPU_REBUILDS_SEALED_STACK")
    require("/src/dependencies/NFD" not in dockerfile, "PREFLIGHT_GPU_REBUILDS_NFD")
    require("preflight-gpu-build.py" in workflow, "PREFLIGHT_WORKFLOW_GATE_MISSING")
    require(workflow.index("preflight-gpu-build.py") < workflow.index("docker/setup-buildx-action"), "PREFLIGHT_WORKFLOW_GATE_LATE")
    require("foundation_image:" in workflow and "FOUNDATION_IMAGE=" in workflow, "PREFLIGHT_FOUNDATION_INPUT_MISSING")
    require(
        "foundation_source_revision:" in workflow
        and "FOUNDATION_SOURCE_REVISION=${{ inputs.foundation_source_revision }}" in workflow,
        "PREFLIGHT_FOUNDATION_SOURCE_INPUT_MISSING",
    )
    require(PYTHON_BASE in workflow and GPU_BUILD_BASE in workflow and GPU_RUNTIME_BASE in workflow, "PREFLIGHT_WORKFLOW_BASE_IMAGE_DRIFT")
    require(re.search(r"(?m)^  push:", workflow) is None, "PREFLIGHT_UNREVIEWED_PUSH_TRIGGER_PRESENT")
    require("prepare-sealed-context.py" not in workflow, "PREFLIGHT_CLOUD_REBUILDS_FOUNDATION")
    require("gh api --method PATCH" not in workflow, "PREFLIGHT_UNSUPPORTED_GHCR_VISIBILITY_MUTATION")
    require("Verify anonymous digest access" in workflow and "DOCKER_CONFIG=\"$anonymous_config\" docker manifest inspect" in workflow, "PREFLIGHT_ANONYMOUS_PULL_GATE_MISSING")
    require(re.search(r"name: Record runner disk after build\n\s+if: always\(\)", workflow) is not None, "PREFLIGHT_FAILURE_EVIDENCE_NOT_ALWAYS")
    archive_count = validate_archives(seal_root, set(sources)) if seal_root else 0
    return {
        "status": "PASS",
        "schemaVersion": "spec110-gpu-build-preflight-v1",
        "sourceCount": len(sources),
        "archiveCount": archive_count,
        "pythonPackageCount": len(python),
        "systemCudaRequirementCount": len(system_python),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", required=True)
    parser.add_argument("--seal")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        report = run(
            Path(args.workspace).resolve(),
            Path(args.seal).resolve() if args.seal else None,
        )
    except (OSError, KeyError, ValueError, tarfile.TarError, PreflightError) as error:
        report = {"status": "FAIL", "reasonCode": str(error)}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    print(text, end="")
    if args.output != "/dev/null":
        Path(args.output).write_text(text, encoding="utf-8")
    return 0 if report["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
