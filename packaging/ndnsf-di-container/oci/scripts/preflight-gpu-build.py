#!/usr/bin/env python3
"""Static, fail-closed preflight for the sealed iTiger GPU OCI build graph."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile


RELIC_REVISION = "b984e901ba78c83ea4093ea96addd13628c8c2d0"
WEBSOCKETPP_REVISION = "ac4e021333675fc80b96eb7be45d218581c897e2"
NDN_SVS_REVISION = "060811333de68b9674e45522222a14d4e047bf28"
NAC_ABE_REVISION = "390e9001a8611e04c90f3a5866d09c3136c885d0"
FOUNDATION_BASE = "docker.io/library/ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214"
PYTHON_BASE = "docker.io/library/python@sha256:b3061b93c8df9809c3783a4f17bbf2520425ec6b40bd3e5e7538870e21ba7209"
GPU_BUILD_BASE = "docker.io/nvidia/cuda@sha256:f18cf1a9ac2842e59f13b0d0729594da8cbd68cadd2379308cdd98c0374dbd80"
GPU_RUNTIME_BASE = "docker.io/nvidia/cuda@sha256:a6a8417cb56c9a5d30c4d8c78ad18bc9b75ffe4453fe1c04b3149b3741518b06"
REQUIRED_SYSTEM = {"bison", "flex", "libfl-dev", "libgtest-dev", "libpcap-dev"}
REQUIRED_PYTHON_RUNTIME = {
    "libgdbm6", "libreadline8", "libsqlite3-0", "libssl1.1",
}
REQUIRED_PYTHON = {
    "certifi", "cffi", "charset-normalizer", "coloredlogs", "cryptography",
    "filelock", "flatbuffers",
    "fsspec", "humanfriendly", "idna", "jinja2", "markupsafe", "mpmath",
    "networkx", "nvidia-cusparselt-cu12", "nvidia-ml-py", "nvidia-nccl-cu12",
    "packaging", "pillow", "protobuf", "pycparser", "pyyaml", "regex", "requests", "sympy", "tqdm",
    "triton", "typing-extensions", "urllib3",
}
REQUIRED_QWEN3_TRANSFORMERS = "4.51.0"
REQUIRED_QWEN3_HUGGINGFACE_HUB = "0.30.2"
REQUIRED_DEPLOYMENT_PYTHON = {"onnxruntime-gpu", "tokenizers"}
DEPLOYMENT_FORBIDDEN_PYTHON = {"torch", "transformers"}
REQUIRED_OFFLINE_EXPORTER = {"torch", "transformers"}
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

REQUIRED_NATIVE_TARGET_USES = {
    "di-native-provider-session-smoke": {
        "BOOST", "NDN_CXX", "NDN_SVS", "PROTOBUF", "NAC-ABE", "NDNSD",
        "OPENSSL", "DL",
    },
    "di-native-plan-onnx-smoke": {"BOOST", "NDN_CXX", "NDN_SVS", "ONNXRUNTIME", "DL"},
    "di-native-onnxruntime-smoke": {"BOOST", "NDN_CXX", "NDN_SVS", "ONNXRUNTIME", "DL"},
    "di-native-provider": {"BOOST", "NDN_CXX", "NDN_SVS", "ONNXRUNTIME", "DL"},
    "di-native-fault-provider": {"BOOST", "NDN_CXX", "NDN_SVS", "ONNXRUNTIME", "DL"},
}

REQUIRED_REPO_TARGET_USES = {
    "DistributedRepoSmoke": {"DL"},
    "DistributedRepoTieredCacheTest": {"DL"},
    "DistributedRepoExactPacketTest": {"DL"},
    "DistributedRepoHaTest": {"DL"},
}


class PreflightError(RuntimeError):
    pass


def require(condition: bool, code: str) -> None:
    if not condition:
        raise PreflightError(code)


def validate_archives(
    seal_root: Path, sources: dict[str, dict[str, object]]
) -> int:
    seal = json.loads((seal_root / "source-seal.json").read_text())
    dependencies = seal.get("dependencies", {})
    require(set(dependencies) == set(sources), "PREFLIGHT_SEAL_SOURCE_SET_MISMATCH")
    checked = 0
    for name, marker in ARCHIVE_MARKERS.items():
        dependency = dependencies[name]
        expected = sources[name]
        require(
            dependency.get("revision") == expected.get("revision"),
            f"PREFLIGHT_SEAL_REVISION_MISMATCH:{name}",
        )
        archive_path = seal_root / dependency["archivePath"]
        require(archive_path.is_file(), f"PREFLIGHT_SEAL_ARCHIVE_MISSING:{name}")
        archive_bytes = archive_path.stat().st_size
        require(
            dependency.get("archiveBytes") == archive_bytes,
            f"PREFLIGHT_SEAL_ARCHIVE_SIZE_MISMATCH:{name}",
        )
        archive_digest = "sha256:" + hashlib.sha256(
            archive_path.read_bytes()
        ).hexdigest()
        require(
            dependency.get("archiveDigest") == archive_digest,
            f"PREFLIGHT_SEAL_ARCHIVE_DIGEST_MISMATCH:{name}",
        )
        with tarfile.open(archive_path, "r:") as archive:
            names = set(archive.getnames())
        require(marker in names, f"PREFLIGHT_ARCHIVE_ENTRY_MISSING:{name}:{marker}")
        checked += 1
    return checked


def validate_workspace_archive(workspace: Path, seal_root: Path) -> None:
    """Require the Git-free workspace archive used on compute nodes."""
    seal = json.loads((seal_root / "source-seal.json").read_text())
    expected = seal.get("workspace", {})
    archive = workspace.parent / "workspace.tar"
    require(archive.is_file(), "PREFLIGHT_WORKSPACE_ARCHIVE_MISSING")
    require(
        expected.get("archiveBytes") == archive.stat().st_size,
        "PREFLIGHT_WORKSPACE_ARCHIVE_SIZE_MISMATCH",
    )
    require(
        expected.get("archiveDigest")
        == "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest(),
        "PREFLIGHT_WORKSPACE_ARCHIVE_DIGEST_MISMATCH",
    )


def validate_seal_lock_digest(workspace: Path, seal_root: Path) -> None:
    """Ensure the sealed manifest was generated from this exact GPU lock."""
    seal = json.loads((seal_root / "source-seal.json").read_text())
    lock = workspace / "packaging/ndnsf-di-container/oci/locks/gpu.lock"
    require(lock.is_file(), "PREFLIGHT_LOCK_MISSING_FOR_SEAL")
    expected = "sha256:" + hashlib.sha256(lock.read_bytes()).hexdigest()
    require(
        seal.get("lockDigest") == expected,
        "PREFLIGHT_SEAL_LOCK_DIGEST_MISMATCH",
    )


def validate_native_target_closure(workspace: Path) -> int:
    """Check every native DI smoke/provider target's explicit Waf closure.

    Waf targets do not inherit ``use=`` entries from sibling targets.  Keep
    this check textual and fail closed before a container build so a selected
    smoke target cannot hide a missing dependency in another target.
    """
    text = (workspace / "examples/wscript").read_text()
    blocks = re.split(r"(?=\n\s*bld\.program\()", text)
    targets: dict[str, set[str]] = {}
    for block in blocks:
        match = re.search(r"bld\.program\(name='([^']+)'", block)
        if not match:
            continue
        use_match = re.search(r"\n\s*use='([^']+)'", block)
        targets[match.group(1)] = set(use_match.group(1).split()) if use_match else set()
    for target, required in REQUIRED_NATIVE_TARGET_USES.items():
        require(target in targets, f"PREFLIGHT_NATIVE_TARGET_MISSING:{target}")
        missing = sorted(required - targets[target])
        require(not missing,
                f"PREFLIGHT_NATIVE_TARGET_USE_MISSING:{target}:{','.join(missing)}")
    for target, uses in targets.items():
        if "OPENSSL" in uses:
            require("DL" in uses,
                    f"PREFLIGHT_OPENSSL_TARGET_USE_MISSING:{target}:DL")
    return len(targets)


def validate_repo_target_closure(workspace: Path) -> int:
    """Require explicit system-library closure for repo example binaries.

    These targets link OpenSSL through the repository library.  The indirect
    ``dlopen`` dependency is not reliably propagated by every OpenSSL
    pkg-config file, so each executable must declare ``DL`` itself.
    """
    text = (workspace / "NDNSF-DistributedRepo/wscript").read_text()
    blocks = re.split(r"(?=\n\s*bld\.program\()", text)
    targets: dict[str, set[str]] = {}
    for block in blocks:
        match = re.search(r"bld\.program\(name='([^']+)'", block)
        if not match:
            continue
        use_match = re.search(r"\n\s*use='([^']+)'", block)
        targets[match.group(1)] = set(use_match.group(1).split()) if use_match else set()
    for target, required in REQUIRED_REPO_TARGET_USES.items():
        require(target in targets, f"PREFLIGHT_REPO_TARGET_MISSING:{target}")
        missing = sorted(required - targets[target])
        require(not missing,
                f"PREFLIGHT_REPO_TARGET_USE_MISSING:{target}:{','.join(missing)}")
    return len(targets)


def validate_framework_link_closure(workspace: Path) -> None:
    """Require the shared framework to export its indirect loader dependency."""
    text = (workspace / "wscript").read_text()
    block = text.split("libndn_service_framework = dict(", 1)[1].split(
        "    if bld.env.enable_shared:", 1
    )[0]
    use_match = re.search(r"\n\s*use='([^']+)'", block)
    require(use_match is not None, "PREFLIGHT_FRAMEWORK_USE_MISSING")
    require("DL" in set(use_match.group(1).split()),
            "PREFLIGHT_FRAMEWORK_USE_MISSING:DL")


def validate_ndn_cxx_link_closure(workspace: Path) -> None:
    """Require the configured NDN-CXX uselib to carry the platform dl ABI."""
    text = (workspace / "wscript").read_text()
    marker = "if 'dl' not in conf.env.LIB_NDN_CXX:"
    require(marker in text, "PREFLIGHT_NDN_CXX_DL_CLOSURE_MISSING")
    require("conf.env.LIB_NDN_CXX.append('dl')" in text,
            "PREFLIGHT_NDN_CXX_DL_CLOSURE_INVALID")


def run(workspace: Path, seal_root: Path | None) -> dict[str, object]:
    oci = workspace / "packaging/ndnsf-di-container/oci"
    lock = json.loads((oci / "locks/gpu.lock").read_text())
    foundation = (oci / "Dockerfile.foundation").read_text()
    dockerfile = (oci / "Dockerfile.gpu").read_text()
    rootless_template = (
        workspace
        / "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/rootless-build.sbatch.in"
    ).read_text()
    rootless_script = (
        workspace
        / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh"
    ).read_text()
    runtime_closure = (oci / "scripts/verify-runtime-closure.py").read_text()
    matrix = (oci / "compatibility/gpu-matrix.yaml").read_text()
    native_target_count = validate_native_target_closure(workspace)
    repo_target_count = validate_repo_target_closure(workspace)
    validate_framework_link_closure(workspace)
    validate_ndn_cxx_link_closure(workspace)
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
    deployment = {name.lower() for name in lock.get("deploymentPythonPackages", {})}
    offline_exporter = {name.lower() for name in lock.get("offlineExporterPackages", {})}
    require(REQUIRED_PYTHON <= python, "PREFLIGHT_PYTHON_CLOSURE_INCOMPLETE")
    require(REQUIRED_DEPLOYMENT_PYTHON <= deployment, "PREFLIGHT_DEPLOYMENT_RUNTIME_INCOMPLETE")
    require(not DEPLOYMENT_FORBIDDEN_PYTHON.intersection(deployment), "PREFLIGHT_DEPLOYMENT_FORBIDDEN_PYTHON")
    require(REQUIRED_OFFLINE_EXPORTER <= offline_exporter, "PREFLIGHT_OFFLINE_EXPORTER_CONTRACT_INCOMPLETE")
    require(
        lock.get("pythonPackages", {}).get("transformers") == REQUIRED_QWEN3_TRANSFORMERS,
        "PREFLIGHT_QWEN3_TRANSFORMERS_VERSION_INVALID",
    )
    require(
        lock.get("pythonPackages", {}).get("huggingface-hub") == REQUIRED_QWEN3_HUGGINGFACE_HUB,
        "PREFLIGHT_QWEN3_HUGGINGFACE_HUB_VERSION_INVALID",
    )
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
        "/bin/bash -lc 'set -eu; cd /src/dependencies/openabe",
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
        "grep -Eq '^[a-f0-9]{40}$'",
        "find NDNSF-DistributedInference/packaging/python -type d -name build",
        "onnxRuntimeExcludedOptionalProviders",
        "/opt/onnxruntime/lib/libonnxruntime_providers_tensorrt.so",
        "/opt/venv/lib/python3.10/site-packages/onnxruntime/capi/libonnxruntime_providers_tensorrt.so",
        "/opt/onnxruntime/lib/libonnxruntime_providers_cuda.so",
        "/opt/venv/lib/python3.10/site-packages/onnxruntime/capi/libonnxruntime_providers_cuda.so",
        'org.ndnsf.di.foundation.revision="${FOUNDATION_SOURCE_REVISION}"',
        "missing_packages", "dpkg-query -W -f='${db:Status-Abbrev}'",
        "grep -qx '.i '",
        "--pip-check",
    )
    for marker in markers:
        require(marker in dockerfile, f"PREFLIGHT_DOCKER_MARKER_MISSING:{marker}")
    require(
        dockerfile.count("--pip-check") >= 2,
        "PREFLIGHT_PIP_CHECK_COVERAGE_INCOMPLETE",
    )
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
        "--foundation-base @@FOUNDATION_BASE@@" in rootless_template,
        "PREFLIGHT_ROOTLESS_FOUNDATION_INPUT_MISSING",
    )
    for marker in (
        "Dockerfile.foundation",
        "FOUNDATION_BASE_IMAGE",
        "--target foundation",
        "foundation_local_image=",
        "ROOTLESS_BUILD_FOUNDATION_FAILED",
        "--pull=missing",
        "FOUNDATION_SOURCE_REVISION=$source_revision",
    ):
        require(marker in rootless_script, f"PREFLIGHT_ROOTLESS_FOUNDATION_BUILD_MISSING:{marker}")
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
    if seal_root:
        validate_seal_lock_digest(workspace, seal_root)
        validate_workspace_archive(workspace, seal_root)
    archive_count = validate_archives(seal_root, sources) if seal_root else 0
    return {
        "status": "PASS",
        "schemaVersion": "spec110-gpu-build-preflight-v1",
        "sourceCount": len(sources),
        "nativeTargetCount": native_target_count,
        "repoTargetCount": repo_target_count,
        "archiveCount": archive_count,
        "pythonPackageCount": len(python),
        "deploymentPythonPackageCount": len(deployment),
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
