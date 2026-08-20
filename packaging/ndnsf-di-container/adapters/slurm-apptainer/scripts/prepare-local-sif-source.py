#!/usr/bin/env python3
"""Create a deterministic, source-only input for the Spec 170 SIF builder."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import subprocess
import tarfile
from pathlib import Path


SCHEMA = "spec170-local-sif-source-v1"
SEAL_DIGEST_BASIS = "path-independent-content-v1"
FILES = (
    "waf",
    "wscript",
    "libndn-service-framework.pc.in",
    ".waf-tools",
    "ndn-service-framework",
    "NDNSF-DistributedInference/cpp",
    "NDNSF-DistributedInference/ndnsf_distributed_inference",
    "NDNSF-DistributedInference/packaging/python",
    "NDNSF-DistributedInference/setup.py",
    "pythonWrapper/setup.py",
    "pythonWrapper/pyproject.toml",
    "pythonWrapper/README.md",
    "pythonWrapper/ndnsf",
    "pythonWrapper/src",
    "examples/wscript",
    "examples/App_ServiceController.cpp",
    "NDNSF-DistributedRepo/include",
)
NDN_SVS_FILES = (
    "waf",
    "wscript",
    "VERSION.info",
    "libndn-svs.pc.in",
    ".waf-tools",
    "ndn-svs",
)
EXCLUDED_DIRS = {"build", "__pycache__", "node_modules"}
EXCLUDED_SUFFIXES = {".so", ".a", ".o", ".pyc", ".pyo"}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def selected_files(workspace: Path) -> list[Path]:
    selected: set[Path] = set()
    for relative in FILES:
        source = workspace / relative
        if not source.exists():
            raise SystemExit(f"LOCAL_SIF_SOURCE_MISSING:{relative}")
        candidates = [source] if source.is_file() else source.rglob("*")
        for candidate in candidates:
            if not candidate.is_file():
                continue
            rel = candidate.relative_to(workspace)
            if any(part in EXCLUDED_DIRS or part.endswith(".egg-info") for part in rel.parts):
                continue
            if candidate.suffix in EXCLUDED_SUFFIXES:
                continue
            selected.add(rel)
    for candidate in (workspace / "examples").glob("DI_Native*"):
        if candidate.is_file() and candidate.suffix not in EXCLUDED_SUFFIXES:
            selected.add(candidate.relative_to(workspace))
    # Waf loads every bld.recurse() file while constructing the target graph,
    # even when --targets selects only a subset. Seal that graph recursively so
    # a missing child wscript cannot survive until the container build.
    pending = [workspace / "wscript"]
    visited: set[Path] = set()
    while pending:
        build_file = pending.pop()
        if build_file in visited:
            continue
        if not build_file.is_file():
            raise SystemExit(
                f"LOCAL_SIF_RECURSED_WSCRIPT_MISSING:"
                f"{build_file.relative_to(workspace)}")
        visited.add(build_file)
        selected.add(build_file.relative_to(workspace))
        tree = ast.parse(build_file.read_text(encoding="utf-8"),
                         filename=str(build_file))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            function = node.func
            if (not isinstance(function, ast.Attribute) or
                    function.attr != "recurse" or not node.args or
                    not isinstance(node.args[0], ast.Constant) or
                    not isinstance(node.args[0].value, str)):
                continue
            child = build_file.parent / node.args[0].value / "wscript"
            pending.append(child.resolve())
    return sorted(selected, key=lambda value: value.as_posix())


def selected_dependency_files(workspace: Path, entries: tuple[str, ...]) -> list[Path]:
    selected: set[Path] = set()
    for relative in entries:
        source = workspace / relative
        if not source.exists():
            raise SystemExit(f"LOCAL_SIF_DEPENDENCY_SOURCE_MISSING:{relative}")
        candidates = [source] if source.is_file() else source.rglob("*")
        for candidate in candidates:
            if not candidate.is_file():
                continue
            rel = candidate.relative_to(workspace)
            if any(part in EXCLUDED_DIRS or part.endswith(".egg-info")
                   for part in rel.parts):
                continue
            if candidate.suffix in EXCLUDED_SUFFIXES:
                continue
            selected.add(rel)
    return sorted(selected, key=lambda value: value.as_posix())


def add_file(archive: tarfile.TarFile, source: Path, relative: Path) -> None:
    info = archive.gettarinfo(str(source), arcname=relative.as_posix())
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = 0
    info.mode = 0o755 if os.access(source, os.X_OK) else 0o644
    with source.open("rb") as stream:
        archive.addfile(info, stream)


def canonical_seal_body(body: dict) -> dict:
    """Return the path-independent representation used for sealDigest.

    Workspace and archive paths are operational locations, not source
    identity.  Keeping them in the JSON is useful for local validation, but
    including them in the content digest made the same source produce a
    different seal when the output directory moved.
    """
    canonical = json.loads(json.dumps(body, sort_keys=True))
    canonical.pop("sealDigest", None)
    canonical["workspace"] = "<workspace>"
    canonical["sealDigestBasis"] = SEAL_DIGEST_BASIS
    archive = canonical.get("archive")
    if isinstance(archive, dict):
        archive["path"] = "<archive:workspace>"
    dependencies = canonical.get("dependencies", {})
    if isinstance(dependencies, dict):
        for name, record in dependencies.items():
            if not isinstance(record, dict):
                continue
            record["workspace"] = f"<workspace:{name}>"
            dep_archive = record.get("archive")
            if isinstance(dep_archive, dict):
                dep_archive["path"] = f"<archive:{name}>"
    return canonical


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--ndn-svs-workspace", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    workspace = args.workspace.resolve()
    output = args.output_dir.resolve()
    archive_path = output / "workspace.tar"
    seal_path = output / "source-seal.json"
    if output.exists() and any(output.iterdir()):
        raise SystemExit("LOCAL_SIF_SOURCE_OUTPUT_NOT_EMPTY")
    output.mkdir(parents=True, exist_ok=True)

    files = selected_files(workspace)
    if not files:
        raise SystemExit("LOCAL_SIF_SOURCE_EMPTY")
    with tarfile.open(archive_path, "w", format=tarfile.PAX_FORMAT) as archive:
        for relative in files:
            add_file(archive, workspace / relative, relative)

    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=workspace, text=True
    ).strip()
    rows = [
        {
            "path": relative.as_posix(),
            "bytes": (workspace / relative).stat().st_size,
            "sha256": digest(workspace / relative),
        }
        for relative in files
    ]
    body = {
        "schemaVersion": SCHEMA,
        "sourceRevision": revision,
        "sourceMode": "sealed-current-worktree-files",
        "workspace": str(workspace),
        "archive": {
            "path": str(archive_path),
            "bytes": archive_path.stat().st_size,
            "sha256": digest(archive_path),
        },
        "fileCount": len(rows),
        "files": rows,
        "compiledPayloadCount": 0,
    }
    dependency_report = None
    if args.ndn_svs_workspace is not None:
        dependency_workspace = args.ndn_svs_workspace.resolve()
        dependency_archive = output / "ndn-svs.tar"
        dependency_files = selected_dependency_files(
            dependency_workspace, NDN_SVS_FILES)
        with tarfile.open(dependency_archive, "w",
                          format=tarfile.PAX_FORMAT) as archive:
            for relative in dependency_files:
                add_file(archive, dependency_workspace / relative, relative)
        dependency_rows = [
            {
                "path": relative.as_posix(),
                "bytes": (dependency_workspace / relative).stat().st_size,
                "sha256": digest(dependency_workspace / relative),
            }
            for relative in dependency_files
        ]
        dependency_report = {
            "sourceRevision": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=dependency_workspace,
                text=True).strip(),
            "sourceMode": "sealed-current-worktree-files",
            "workspace": str(dependency_workspace),
            "archive": {
                "path": str(dependency_archive),
                "bytes": dependency_archive.stat().st_size,
                "sha256": digest(dependency_archive),
            },
            "fileCount": len(dependency_rows),
            "files": dependency_rows,
            "compiledPayloadCount": 0,
        }
        body["dependencies"] = {"ndnSvs": dependency_report}
    body["sealDigestBasis"] = SEAL_DIGEST_BASIS
    body["sealDigest"] = "sha256:" + hashlib.sha256(
        json.dumps(canonical_seal_body(body), sort_keys=True,
                   separators=(",", ":")).encode()
    ).hexdigest()
    seal_path.write_text(json.dumps(body, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    print(json.dumps({
        "status": "PASS",
        "archive": str(archive_path),
        "archiveSha256": body["archive"]["sha256"],
        "fileCount": len(rows),
        "ndnSvsArchive": (
            dependency_report["archive"] if dependency_report else None),
        "seal": str(seal_path),
        "sealDigest": body["sealDigest"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
