#!/usr/bin/env python3
"""Generate the local/MiniNDN-only Spec 111 candidate identity.

The identity deliberately excludes test/evidence documents so that final gate
reports do not mutate the candidate they describe.  Runtime/container fields
remain immutable deferral markers; Spec 110 must create a different candidate.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
OUTPUT = REPO / (
    "specs/111-ndnsf-di-core-app-separation/evidence/"
    "post-separation-candidate.json"
)
RUNTIME_ROOTS = (
    "NDNSF-DistributedInference/cpp",
    "NDNSF-DistributedInference/ndnsf_distributed_inference",
    "NDNSF-DistributedInference/packaging",
    "NDNSF-DistributedInference/setup.py",
    "ndn-service-framework",
    "pythonWrapper",
    "examples",
    "Experiments",
    "packaging/ndnsf-di-container",
    "wscript",
)
EXCLUDED_PARTS = {
    "__pycache__",
    "build",
    "builddir",
    ".pytest_cache",
}
EXCLUDED_SUFFIXES = {".pyc", ".pyo", ".so", ".o", ".a", ".log"}
EXCLUDED_PREFIXES = (
    "Experiments/gRPC/",
    "NDNSF-DistributedInference/packaging/python/compat/src/",
)
DEFERRED = "DEFERRED_TO_SPEC110"


def sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=REPO, text=True
    ).strip()


def runtime_files() -> list[Path]:
    selected: set[Path] = set()
    for root_name in RUNTIME_ROOTS:
        root = REPO / root_name
        if root.is_file():
            selected.add(root)
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.is_symlink():
                continue
            relative = path.relative_to(REPO)
            if any(part in EXCLUDED_PARTS for part in relative.parts):
                continue
            if relative.as_posix().startswith(EXCLUDED_PREFIXES):
                continue
            if path.suffix in EXCLUDED_SUFFIXES:
                continue
            selected.add(path)
    return sorted(selected, key=lambda path: path.relative_to(REPO).as_posix())


def tree_manifest() -> tuple[list[dict[str, object]], str]:
    rows: list[dict[str, object]] = []
    digest = hashlib.sha256()
    for path in runtime_files():
        relative = path.relative_to(REPO).as_posix()
        file_digest = sha256_file(path)
        row = {
            "path": relative,
            "bytes": path.stat().st_size,
            "sha256": file_digest,
        }
        rows.append(row)
        digest.update(relative.encode("utf-8") + b"\0")
        digest.update(str(row["bytes"]).encode("ascii") + b"\0")
        digest.update(file_digest.encode("ascii") + b"\n")
    return rows, "sha256:" + digest.hexdigest()


def main() -> int:
    files, source_digest = tree_manifest()
    native_extensions = sorted(
        (REPO / "pythonWrapper/ndnsf").glob("_ndnsf*.so")
    )
    if not native_extensions:
        raise SystemExit("SPEC111_NATIVE_EXTENSION_MISSING")
    native = native_extensions[0]
    dependency_paths = [
        REPO / "wscript",
        REPO / "NDNSF-DistributedInference/setup.py",
        REPO / "NDNSF-DistributedInference/packaging/python/core/pyproject.toml",
        REPO / "NDNSF-DistributedInference/packaging/python/sdk/pyproject.toml",
        REPO / "NDNSF-DistributedInference/packaging/python/app/pyproject.toml",
        REPO / "NDNSF-DistributedInference/packaging/python/planner/pyproject.toml",
    ]
    dependency_rows = [
        {
            "path": path.relative_to(REPO).as_posix(),
            "sha256": sha256_file(path),
        }
        for path in dependency_paths
    ]
    dependency_digest = sha256_bytes(
        json.dumps(dependency_rows, sort_keys=True, separators=(",", ":")).encode()
    )
    workflow_gate = REPO / "specs/111-ndnsf-di-core-app-separation/evidence/us4-deployment-workflow-gate.md"
    external_summary = REPO / (
        "results/spec111-us2-external-optimizer-4d695ce8b7ff-bb32fe4c/"
        "summary.json"
    )
    candidate_id = (
        "spec111-local-"
        + source_digest.split(":", 1)[-1][:12]
        + "-"
        + sha256_file(native).split(":", 1)[-1][:12]
    )
    payload = {
        "schema": "ndnsf-di-spec111-candidate-identity-v1",
        "candidateId": candidate_id,
        "sourceSpec": "111",
        "source": {
            "repository": str(REPO),
            "branch": git("branch", "--show-current"),
            "baseCommit": git("rev-parse", "HEAD"),
            "runtimeTreeSha256": source_digest,
            "runtimeFileCount": len(files),
            "runtimeManifestGenerator": (
                "tools/spec111/generate_candidate_identity.py"
            ),
        },
        "dependencies": {
            "manifestSha256": dependency_digest,
            "manifests": dependency_rows,
            "python": platform.python_version(),
            "platform": platform.platform(),
        },
        "localBuild": {
            "nativeExtension": native.relative_to(REPO).as_posix(),
            "nativeExtensionSha256": sha256_file(native),
            "wafBuildResult": "PASS_334_ACTIONS",
        },
        "configuration": {
            "distributedRuntime": "MiniNDN",
            "modelRuntime": "fake-deterministic-3-stage-24-layer",
            "policySuite": "ndnsf-di-default-v1",
            "modelWeightsIncluded": False,
        },
        "lineage": {
            "historicalClaimsInherited": False,
            "promotedFrom": "",
            "evidenceEpoch": "spec111-post-separation-local-v1",
            "workflowGateSha256": sha256_file(workflow_gate),
            "externalOptimizerMiniNdnSummarySha256": (
                sha256_file(external_summary) if external_summary.is_file() else None
            ),
        },
        "releaseDigest": DEFERRED,
        "ociDigest": DEFERRED,
        "sifDigest": DEFERRED,
        "containerBuildDigest": DEFERRED,
        "itigerExecutionDigest": DEFERRED,
        "authorization": {
            "localStatic": True,
            "MiniNDN": True,
            "containerRuntime": False,
            "iTigerSlurm": False,
        },
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    os.chmod(OUTPUT, 0o644)
    print(candidate_id)
    print(sha256_file(OUTPUT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
