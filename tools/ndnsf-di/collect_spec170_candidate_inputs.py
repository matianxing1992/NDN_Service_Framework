#!/usr/bin/env python3
"""Inventory executable Spec170 candidate inputs before the formal freeze.

This tool is deliberately a pre-freeze inventory, not a T029 freeze command.
It binds source, build, harness, test, and job/configuration inputs to one
deterministic digest while excluding generated outputs, caches, raw results,
and historical evidence.  The inventory is useful for deciding whether a
source-only SIF seal is complete; it does not authorize a build or a Tiger
submission.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Iterable


SCHEMA = "spec170-candidate-input-inventory-v1"

# These roots are the inputs named by T024/T025/T028 and the runtime source
# trees used by the native and Python paths.  Evidence artifacts are supplied
# explicitly by the freeze process and are intentionally not swept here.
DEFAULT_ROOTS = (
    "waf",
    "wscript",
    "ndn-service-framework",
    "NDNSF-DistributedInference/cpp",
    "NDNSF-DistributedInference/ndnsf_distributed_inference",
    "NDNSF-DistributedInference/packaging/python",
    "NDNSF-DistributedInference/setup.py",
    "pythonWrapper",
    "examples",
    "Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py",
    "Experiments/NDNSF_DI_LlmPipeline_Minindn.py",
    "Experiments/NDNSF_DI_NativeTracer_Minindn.py",
    "Experiments/NDNSF_NewAPI_Minindn_Perf.py",
    "Experiments/NDNSF_LargeData_NacAbe_Minindn.py",
    "Experiments/spec170_dependency_evidence.py",
    "Experiments/analyze_spec171_opportunity_holdout.py",
    "Experiments/run_spec171_provider_transition.py",
    "Experiments/Topology/AI_Lab.conf",
    "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts",
    "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates",
    "packaging/ndnsf-di-container/lib",
    "packaging/ndnsf-di-container/oci",
    "tests/unit-tests",
    "tests/integration-tests",
    "tests/python",
    "tests/container",
    "specs/170-reusable-layer-artifacts/contracts",
    "specs/170-reusable-layer-artifacts/jobs",
    "specs/170-reusable-layer-artifacts/spec.md",
    "specs/170-reusable-layer-artifacts/plan.md",
    "specs/170-reusable-layer-artifacts/tasks.md",
)

EXCLUDED_PARTS = {
    ".git",
    "__pycache__",
    "build",
    "dist",
    "node_modules",
    "results",
}
EXCLUDED_SUFFIXES = {".a", ".o", ".pyc", ".pyo", ".so"}


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def excluded(relative: Path) -> bool:
    if any(part.startswith(".") or part in EXCLUDED_PARTS or
           part.endswith(".egg-info")
           for part in relative.parts):
        return True
    return relative.suffix in EXCLUDED_SUFFIXES


def iter_inputs(root: Path, relative: Path) -> Iterable[Path]:
    source = root / relative
    if not source.exists() and not source.is_symlink():
        raise SystemExit(f"SPEC170_CANDIDATE_INPUT_MISSING:{relative}")
    if source.is_file() or source.is_symlink():
        if not excluded(relative):
            yield relative
        return
    for candidate in sorted(source.rglob("*")):
        candidate_relative = candidate.relative_to(root)
        if excluded(candidate_relative):
            continue
        if candidate.is_file() or candidate.is_symlink():
            yield candidate_relative


def git_status_digest(root: Path) -> tuple[str, int]:
    status = subprocess.check_output(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"],
        cwd=root,
    )
    return digest_bytes(status), len(status.splitlines())


def file_row(root: Path, relative: Path, role: str) -> dict[str, object]:
    path = root / relative
    if path.is_symlink():
        target = os.readlink(path)
        return {
            "path": relative.as_posix(),
            "role": role,
            "kind": "symlink",
            "target": target,
            "mode": path.lstat().st_mode & 0o7777,
        }
    return {
        "path": relative.as_posix(),
        "role": role,
        "kind": "file",
        "bytes": path.stat().st_size,
        "sha256": digest_file(path),
        "mode": path.stat().st_mode & 0o7777,
        "executable": os.access(path, os.X_OK),
    }


def build_inventory(root: Path, roots: tuple[str, ...]) -> dict[str, object]:
    selected: dict[str, tuple[str, Path]] = {}
    for configured in roots:
        relative = Path(configured)
        for path in iter_inputs(root, relative):
            key = path.as_posix()
            selected.setdefault(key, (configured, path))

    rows = [file_row(root, path, role) for role, path in
            (selected[key] for key in sorted(selected))]
    status_digest, status_rows = git_status_digest(root)
    body: dict[str, object] = {
        "schemaVersion": SCHEMA,
        "sourceRevision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "sourceMode": "pre-freeze-current-worktree-inputs",
        "roots": list(roots),
        "excludedParts": sorted(EXCLUDED_PARTS),
        "excludedSuffixes": sorted(EXCLUDED_SUFFIXES),
        "worktreeStatusSha256": status_digest,
        "worktreeStatusRows": status_rows,
        "fileCount": len(rows),
        "files": rows,
    }
    body["inventoryDigest"] = digest_bytes(canonical_json(body))
    return body


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--root", action="append", dest="roots",
        help="override the default roots; repeatable")
    args = parser.parse_args(argv)
    workspace = args.workspace.resolve()
    roots = tuple(dict.fromkeys(args.roots or DEFAULT_ROOTS))
    body = build_inventory(workspace, roots)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(body, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(json.dumps({
        "status": "PASS",
        "schemaVersion": SCHEMA,
        "fileCount": body["fileCount"],
        "worktreeStatusRows": body["worktreeStatusRows"],
        "inventoryDigest": body["inventoryDigest"],
        "output": str(output),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
