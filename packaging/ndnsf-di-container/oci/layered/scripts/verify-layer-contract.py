#!/usr/bin/env python3
"""Static fail-closed verification of the Spec 158 layer ownership contract."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


class ContractError(RuntimeError):
    pass


def require(condition: bool, code: str) -> None:
    if not condition:
        raise ContractError(code)


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def run(workspace: Path) -> dict[str, object]:
    root = workspace / "packaging/ndnsf-di-container/oci"
    layered = root / "layered"
    old = json.loads((root / "locks/gpu.lock").read_text())
    platform = json.loads((layered / "locks/platform.lock.json").read_text())
    ml = json.loads((layered / "locks/ml-runtime.lock.json").read_text())
    ndn = json.loads((layered / "locks/ndn-foundation.lock.json").read_text())
    app = json.loads((layered / "locks/app-runtime.lock.json").read_text())
    docker_ml = (layered / "Dockerfile.ml").read_text()
    docker_ndn = (layered / "Dockerfile.ndn").read_text()
    docker_app = (layered / "Dockerfile.app").read_text()
    boost_patch = layered / "patches/ndn-svs-boost-1.71.patch"

    for value, name in (
        (platform, "platform"),
        (ml, "ml-runtime"),
        (ndn, "ndn-foundation"),
        (app, "app-runtime"),
    ):
        require(value.get("schemaVersion") == "ndnsf-di-layer-lock-v1",
                f"LAYER_LOCK_SCHEMA_INVALID:{name}")
        require(value.get("layer") == name, f"LAYER_LOCK_OWNER_INVALID:{name}")

    require(platform["baseImages"] == {
        "python": old["baseImages"]["python"],
        "gpuBuild": old["baseImages"]["gpuRuntime"],
        "gpuRuntime": old["baseImages"]["gpuRuntime"],
    }, "PLATFORM_BASE_IMAGE_DRIFT")
    require(
        platform["baseImages"]["gpuBuild"] == platform["baseImages"]["gpuRuntime"],
        "UNNECESSARY_CUDA_DEVEL_BASE",
    )
    # The layered ML lock owns only the ML layer.  The legacy GPU lock also
    # carries the foundation's crypto wheel closure, so exact dictionary
    # equality would falsely reject the intentional layer split.  Shared
    # package versions must still be byte-for-byte equal.
    for name, version in ml["pythonPackages"].items():
        require(old["pythonPackages"].get(name) == version,
                f"ML_PYTHON_LOCK_VERSION_DRIFT:{name}")
    require(
        set(old["pythonPackages"]) - set(ml["pythonPackages"])
        == {"cffi", "cryptography", "pycparser"},
        "ML_PYTHON_LOCK_OWNER_SPLIT_INVALID",
    )
    deployment = {name.lower() for name in ml.get("deploymentPythonPackages", {})}
    require({"onnxruntime-gpu", "tokenizers"} <= deployment,
            "ML_DEPLOYMENT_PYTHON_CLOSURE_INCOMPLETE")
    require(not deployment.intersection({"torch", "transformers"}),
            "ML_DEPLOYMENT_FORBIDDEN_PYTHON_PRESENT")
    exporter = {name.lower() for name in ml.get("offlineExporterPackages", {})}
    require({"torch", "transformers"} <= exporter,
            "ML_OFFLINE_EXPORTER_CONTRACT_INCOMPLETE")
    require(
        ml["pythonSystemProvidedPackages"] == old["pythonSystemProvidedPackages"],
        "ML_SYSTEM_CUDA_LOCK_DRIFT",
    )
    require(ml["onnxRuntimeCpp"] == old["onnxRuntimeCpp"], "ML_ORT_LOCK_DRIFT")
    stable_expected = set(old["sourceRepositories"]) - {"ndn-svs", "NDNSD"}
    require(set(ndn["sourceRepositories"]) == stable_expected,
            "NDN_STABLE_SOURCE_SET_INVALID")
    require("NDNSD" in app["sources"] and "ndn-svs" in app["sources"],
            "APP_DEPENDENCY_CHAIN_INCOMPLETE")
    patch_contract = app["buildCompatibilityPatches"]["ndn-svs-boost-1.71"]
    require(
        patch_contract["path"]
        == "packaging/ndnsf-di-container/oci/layered/patches/ndn-svs-boost-1.71.patch",
        "NDN_SVS_BOOST_PATCH_PATH_INVALID",
    )
    require(boost_patch.is_file(), "NDN_SVS_BOOST_PATCH_MISSING")
    require(
        hashlib.sha256(boost_patch.read_bytes()).hexdigest()
        == patch_contract["sha256"],
        "NDN_SVS_BOOST_PATCH_DIGEST_INVALID",
    )
    patch_text = boost_patch.read_text()
    removed_lines = {
        line[1:].strip()
        for line in patch_text.splitlines()
        if line.startswith("-") and not line.startswith("---")
    }
    added_lines = {
        line[1:].strip()
        for line in patch_text.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    }
    for old_text, new_text in (
        ("BOOST_VERSION_NUMBER < 107400", "BOOST_VERSION_NUMBER < 107100"),
        (
            "minimum supported version of Boost is 1.74.0",
            "minimum supported version of Boost is 1.71.0",
        ),
    ):
        require(sum(old_text in line for line in removed_lines) == 1,
                f"NDN_SVS_BOOST_PATCH_OLD_CHANGE_INVALID:{old_text}")
        require(sum(new_text in line for line in added_lines) == 1,
                f"NDN_SVS_BOOST_PATCH_NEW_CHANGE_INVALID:{new_text}")
    require(app["sources"]["NDNSD"]["revision"] ==
            old["sourceRepositories"]["NDNSD"]["revision"],
            "APP_NDNSD_REVISION_DRIFT")
    require("NDNSD" not in ndn["buildOrder"], "NDNSD_WRONG_LAYER")
    require(app["cpuFallbackAllowed"] is False and
            platform["cpuFallbackAllowed"] is False,
            "CPU_FALLBACK_POLICY_INVALID")

    for marker in (
        "FROM ${GPU_BUILD_BASE_IMAGE} AS ml-devel",
        "FROM ${GPU_RUNTIME_BASE_IMAGE} AS ml-runtime",
        "verify-python-environment.py",
        "verify-runtime-closure.py",
        "deploymentPythonPackages",
        "/opt/runtime-venv",
        "import onnxruntime,tokenizers",
        "import torch",
        "import transformers",
        "models-included=\"false\"",
    ):
        require(marker in docker_ml, f"ML_DOCKER_MARKER_MISSING:{marker}")
    for marker in (
        "FROM ${ML_DEVEL_IMAGE} AS ndn-devel",
        "FROM ${ML_RUNTIME_IMAGE} AS ndn-runtime",
        "/opt/ndn-base",
        "test ! -e $PREFIX/lib/pkgconfig/libndn-svs.pc",
        "test ! -e $PREFIX/lib/pkgconfig/ndnsd.pc",
    ):
        require(marker in docker_ndn, f"NDN_DOCKER_MARKER_MISSING:{marker}")
    require("/src/dependencies/ndn-svs" not in docker_ndn, "NDN_REBUILDS_NDN_SVS")
    require("/src/dependencies/NDNSD" not in docker_ndn, "NDN_REBUILDS_NDNSD")
    for marker in (
        "FROM ${NDN_DEVEL_IMAGE} AS app-builder",
        "FROM ${NDN_RUNTIME_IMAGE} AS app-runtime",
        "cd /src/ndn-svs",
        "cd /src/NDNSD",
        "cd /src/ndnsf",
        "/opt/ndnsf-app",
        "USER 65532:65532",
        "NDNSF_ALLOW_CPU_FALLBACK=0",
        "NDN_SVS_BOOST_PATCH_DIGEST_MISMATCH",
        "patch --batch --forward --fuzz=0 -p1",
        "--targets=ndn-service-framework,libndn-service-framework.pc,App_ServiceController,di-native-provider",
        "$APP_PREFIX/lib/libndn-service-framework.so.0.1.0",
        "CPLUS_INCLUDE_PATH=$APP_PREFIX/include:/opt/ndn-base/include",
        "NDNSF_LIBRARY_DIR=$APP_PREFIX/lib",
        "onnx/backend/test/data",
    ):
        require(marker in docker_app, f"APP_DOCKER_MARKER_MISSING:{marker}")
    require(
        docker_app.index("cd /src/ndn-svs")
        < docker_app.index("cd /src/NDNSD")
        < docker_app.index("cd /src/ndnsf"),
        "APP_DEPENDENCY_BUILD_ORDER_INVALID",
    )
    builder_text = docker_app.split("FROM ${NDN_RUNTIME_IMAGE}", 1)[0]
    require(
        builder_text.index("ARG APP_BUILD_ID")
        > builder_text.index("/opt/venv/bin/pip install"),
        "APP_BUILD_ID_INVALIDATES_COMPILATION_CACHE",
    )
    return {
        "status": "PASS",
        "schemaVersion": "spec158-layer-contract-v1",
        "lockDigests": {
            path.name: digest(path)
            for path in sorted((layered / "locks").glob("*.json"))
        },
        "stableSourceCount": len(ndn["sourceRepositories"]),
        "appSourceCount": len(app["sources"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    try:
        report = run(Path(args.workspace).resolve())
    except (OSError, KeyError, ValueError, json.JSONDecodeError, ContractError) as error:
        report = {"status": "FAIL", "reasonCode": str(error)}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    print(text, end="")
    if args.output:
        Path(args.output).write_text(text)
    return 0 if report["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
