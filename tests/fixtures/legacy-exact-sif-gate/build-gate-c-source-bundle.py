#!/usr/bin/env python3
"""Build the immutable full Python/job overlay for Spec 168 Gates C-E."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil


ROOT = Path(__file__).resolve().parents[3]
JOBS = Path(__file__).resolve().parent
THREE_NODE = ROOT / "tests/fixtures/legacy-tiger-three-node"
SCRIPTS = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts"
DI = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference"
NDNSF = ROOT / "pythonWrapper/ndnsf"
REPO = ROOT / "NDNSF-DistributedRepo/pythonWrapper/py_repoclient"
PIPELINE = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"



def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sources(
    native_core_library: Path | None = None,
    native_python_extension: Path | None = None,
    repo_native_python_extension: Path | None = None,
) -> dict[str, Path]:
    selected = {
        "jobs/spec168_exact_sif_cuda_preflight.py":
            JOBS / "spec168_exact_sif_cuda_preflight.py",
        "jobs/gate-c-exact-sif-cuda.sbatch": THREE_NODE / "gate-c-exact-sif-cuda.sbatch.txt",
        "jobs/spec168-overlay-entrypoint.sh":
            THREE_NODE / "spec168-overlay-entrypoint.sh.txt",
        "jobs/spec168_verify_source_bundle.py":
            JOBS / "spec168_verify_source_bundle.py.txt",
        "jobs/spec168_campaign.py": SCRIPTS / "freeze-tiger-campaign.py",
        "jobs/spec168-three-node-rank.sh": THREE_NODE / "spec168-three-node-rank.sh.txt",
        "jobs/spec168-three-node-rank-inner.sh":
            THREE_NODE / "spec168-three-node-rank-inner.sh.txt",
        "jobs/spec168-three-node-analyzer.py":
            SCRIPTS / "analyze-tiger-three-node.py",
        "jobs/spec168-control-plane-canary-analyzer.py":
            SCRIPTS / "analyze-tiger-control-plane-canary.py",
        "jobs/spec168-prepare-single-campaign.py":
            SCRIPTS / "prepare-tiger-single-campaign.py",
        "jobs/spec168-compat-child-abi-check.py":
            THREE_NODE / "spec168-compat-child-abi-check.py.txt",
        "jobs/spec168-cold-warm-analyzer.py":
            SCRIPTS / "analyze-tiger-cold-warm.py",
        "jobs/spec168_cold_warm_contract.py":
            SCRIPTS / "check-tiger-cold-warm-contract.py",
        "jobs/gate-e-small-single.sbatch": THREE_NODE / "gate-e-small-single.sbatch.txt",
        "jobs/gate-e-small-repeated.sbatch":
            ROOT / "tests/fixtures/legacy-tiger-cold-warm/gate-e-small-repeated.sbatch.txt",
        "jobs/build-automatic-planning-manifest.py":
            SCRIPTS / "build-qwen-planning-manifest.py",
        "jobs/build-generation-policy.py":
            JOBS / "build-generation-policy.py.txt",
        "jobs/run-repo-node.py": JOBS / "run-repo-node.py.txt",
        "jobs/prepare-qwen36.py": JOBS / "prepare-qwen36.py.txt",
        "jobs/register-qwen36-repo.py":
            JOBS / "register-qwen36-repo.py.txt",
        "jobs/nfd.conf.in": ROOT / "tests/fixtures/legacy-qwen-sif-rendering/nfd.conf.in",
        "compat/spec162/generation-rank-inner.sh":
            THREE_NODE / "generation-rank-inner.sh.txt",
        "runtime/core-contracts.py": DI / "core/contracts.py",
        "runtime/core-init.py": DI / "core/__init__.py",
        "runtime/placement.py": DI / "app_sdk/placement.py",
        "runtime/di-provider.py": DI / "provider.py",
        "runtime/ndnsf-service.py": NDNSF / "service.py",
        "runtime/minindn-harness.py":
            ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py",
        "runtime/spec168-real-model-gate.py":
            ROOT / "tools/ndnsf-di/spec168_real_model_gate.py",
        "runtime/spec168-runtime-evidence.py":
            ROOT / "tools/ndnsf-di/spec168_runtime_evidence.py",
    }
    # Overlay the complete DI Python package, not a hand-picked subset. Package
    # __init__ modules import transitive siblings; a partial overlay can combine
    # new exports with stale SIF modules and pass a non-exact local PYTHONPATH
    # test while failing in the exact container.
    for path in DI.rglob("*"):
        if path.is_file() and "__pycache__" not in path.parts:
            selected[f"di/ndnsf_distributed_inference/{path.relative_to(DI)}"] = path
    for prefix, package in (("ndnsf/ndnsf", NDNSF),
                            ("repo/py_repoclient", REPO),
                            ("llm_pipeline", PIPELINE)):
        for path in package.rglob("*"):
            if (path.is_file() and "__pycache__" not in path.parts
                    and path.suffix not in {".pyc", ".o", ".so"}):
                selected[f"{prefix}/{path.relative_to(package)}"] = path
    if native_core_library is not None:
        selected["native/lib/libndn-service-framework.so.0.1.0"] = (
            native_core_library
        )
    if native_python_extension is not None:
        if not re.fullmatch(
            r"_ndnsf\.cpython-310-[A-Za-z0-9_-]+\.so",
            native_python_extension.name,
        ):
            raise ValueError("NATIVE_PYTHON_EXTENSION_NOT_CPYTHON310")
        selected[f"native/python/{native_python_extension.name}"] = (
            native_python_extension
        )
    if repo_native_python_extension is not None:
        if not re.fullmatch(
            r"_py_repoclient\.cpython-310-[A-Za-z0-9_-]+\.so",
            repo_native_python_extension.name,
        ):
            raise ValueError("REPO_NATIVE_PYTHON_EXTENSION_NOT_CPYTHON310")
        selected[
            f"repo/py_repoclient/{repo_native_python_extension.name}"
        ] = repo_native_python_extension
    return selected


def validate_bundle_closure(selected: dict[str, Path]) -> None:
    """Reject direct/transitive file references absent from the bundle."""
    targets = set(selected)
    missing: set[tuple[str, str]] = set()
    for target, source in selected.items():
        if Path(target).suffix not in {".py", ".sh", ".sbatch"}:
            continue
        text = source.read_text(encoding="utf-8")
        for relative in re.findall(
            r"/source/([A-Za-z0-9_./-]+(?:\.py|\.sh|\.sbatch|\.in))",
            text,
        ):
            if relative not in targets:
                missing.add((target, relative))
        for name in re.findall(
            r"Path\(__file__\)\.with_name\([\"']([^\"']+)[\"']\)",
            text,
        ):
            required = str(Path(target).with_name(name))
            if required not in targets:
                missing.add((target, required))
    if missing:
        details = ",".join(f"{owner}->{required}" for owner, required in sorted(missing))
        raise ValueError(f"SOURCE_BUNDLE_CLOSURE_MISSING:{details}")


def build(
    output: Path,
    native_core_library: Path | None = None,
    native_python_extension: Path | None = None,
    repo_native_python_extension: Path | None = None,
) -> str:
    native_closure = (
        native_core_library,
        native_python_extension,
        repo_native_python_extension,
    )
    if any(path is not None for path in native_closure) and not all(
            path is not None for path in native_closure):
        raise ValueError(
            "NATIVE_ABI_CLOSURE_REQUIRES_CORE_NDNSF_AND_REPO_EXTENSIONS")
    if output.exists() and any(output.iterdir()):
        raise ValueError("OUTPUT_DIRECTORY_NOT_EMPTY")
    output.mkdir(parents=True, exist_ok=True)
    selected = sources(
        native_core_library,
        native_python_extension,
        repo_native_python_extension,
    )
    validate_bundle_closure(selected)
    rows = []
    modes: dict[str, str] = {}
    for relative, source in sorted(selected.items()):
        if not source.is_file():
            raise FileNotFoundError(source)
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if (relative.startswith("native/") and
                source.stat().st_mode & 0o222 == 0):
            try:
                os.link(source, target)
            except OSError:
                shutil.copyfile(source, target)
        else:
            shutil.copyfile(source, target)
        mode = 0o555 if target.suffix in {".py", ".sh", ".sbatch"} else 0o444
        target.chmod(mode)
        modes[relative] = f"{mode:04o}"
        rows.append(f"{sha256_file(target)}  {relative}\n")
    modes["source-modes.json"] = "0444"
    mode_manifest = output / "source-modes.json"
    mode_manifest.write_text(
        json.dumps(modes, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    mode_manifest.chmod(0o444)
    rows.append(f"{sha256_file(mode_manifest)}  source-modes.json\n")
    manifest = output / "source-files.sha256"
    manifest.write_text("".join(rows), encoding="utf-8")
    manifest.chmod(0o444)
    return "sha256:" + sha256_file(manifest)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--native-core-library", type=Path)
    parser.add_argument("--native-python-extension", type=Path)
    parser.add_argument("--repo-native-python-extension", type=Path)
    args = parser.parse_args()
    for path in (
        args.native_core_library,
        args.native_python_extension,
        args.repo_native_python_extension,
    ):
        if path is not None and not path.is_file():
            raise FileNotFoundError(path)
    digest = build(
        args.output,
        args.native_core_library,
        args.native_python_extension,
        args.repo_native_python_extension,
    )
    print(f"SPEC168_GATE_C_SOURCE_BUNDLE_PASS digest={digest} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
