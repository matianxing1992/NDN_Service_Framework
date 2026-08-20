#!/usr/bin/env python3
"""Create a deterministic Spec174 authority/source manifest.

This is an evidence helper, not a runtime owner.  It records the immutable
authority inputs, repository identity, dependency lock identity, toolchain
versions, and an explicit dirty-worktree classification.  Generated output is
intentionally stable: timestamps and host-specific paths are omitted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
from typing import Iterable


MANIFEST_SCHEMA = "spec174-source-manifest-v1"
PDF = Path("docs/NDNSFDI/slides/main.pdf")
FEATURE = Path(".specify/feature.json")
SPEC_ROOT = Path("specs/174-ndnsf-di-verified-delivery")
LOCK = Path("packaging/ndnsf-di-container/oci/locks/gpu.lock")

# Results are evidence, not source inputs.  Ignoring this prefix makes a
# second calculation byte-identical after the first manifest is written.
RESULT_PREFIX = "results/spec174/"


def digest_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def digest_file(path: Path) -> str:
    return digest_bytes(path.read_bytes())


def run(repo: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=repo, text=True, stderr=subprocess.STDOUT
    ).strip()


def command(*args: str) -> str:
    try:
        return subprocess.check_output(
            list(args), text=True, stderr=subprocess.STDOUT
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        return f"UNAVAILABLE:{shlex.join(args)}:{exc}"


def tracked_paths(repo: Path) -> list[str]:
    raw = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=repo
    )
    return sorted(item.decode() for item in raw.split(b"\0") if item)


def dirty_entries(repo: Path) -> list[tuple[str, str]]:
    raw = subprocess.check_output(
        ["git", "status", "--porcelain=v1", "-z", "--untracked-files=all"],
        cwd=repo,
    )
    entries: list[tuple[str, str]] = []
    for item in raw.split(b"\0"):
        if not item:
            continue
        value = item.decode(errors="replace")
        status = value[:2]
        path = value[3:] if len(value) > 3 else ""
        # Rename records have a second NUL path.  Preserve the first path and
        # classify the entry as excluded unless an in-scope path is explicit.
        if path.startswith(RESULT_PREFIX):
            continue
        entries.append((status, path))
    return sorted(entries, key=lambda item: item[1])


def is_excluded(path: str) -> bool:
    parts = Path(path).parts
    name = Path(path).name.lower()
    if path.startswith(("docs/", "specs/", ".specify/")) or "/docs/" in path:
        return True
    if any(part.startswith(".") for part in Path(path).parts):
        return True
    if name.startswith("readme"):
        return True
    if Path(path).suffix.lower() in {".pdf", ".tex", ".pptx", ".docx", ".rst"}:
        return True
    if path in {
        "scripts/context_mode_guard.py",
        "tests/python/test_context_mode_guard.py",
    }:
        return True
    return bool(parts and parts[0] in {"docs", "specs"})


def dependency_records(repo: Path, lock: dict) -> dict:
    records: dict[str, dict[str, object]] = {}
    locations = {
        "ndn-cxx": repo.parent / "ndn-cxx",
        "NFD": repo.parent / "NFD",
        "NDNSD": repo.parent / "NDNSD",
        "ndn-svs": repo.parent / "ndn-svs",
        "NAC-ABE": repo.parent / "NAC-ABE",
    }
    for name, expected in sorted(lock["sourceRepositories"].items()):
        path = locations.get(name)
        record: dict[str, object] = {
            "lockedRevision": expected["revision"],
            "pathPresent": bool(path and (path / ".git").exists()),
        }
        if path and (path / ".git").exists():
            record["checkoutRevision"] = run(path, "rev-parse", "HEAD")
            record["checkoutBranch"] = run(path, "branch", "--show-current")
            record["checkoutTrackedClean"] = not bool(
                run(path, "status", "--porcelain", "--untracked-files=no")
            )
            record["checkoutMatchesLock"] = (
                record["checkoutRevision"] == expected["revision"]
            )
        else:
            record["checkoutRevision"] = "UNAVAILABLE"
            record["checkoutBranch"] = "UNAVAILABLE"
            record["checkoutTrackedClean"] = False
            record["checkoutMatchesLock"] = False
        records[name] = record
    return records


def build(repo: Path) -> dict:
    feature = json.loads((repo / FEATURE).read_text(encoding="utf-8"))
    if feature.get("feature_directory") != str(SPEC_ROOT):
        raise SystemExit("SPEC174_FEATURE_POINTER_MISMATCH")
    lock = json.loads((repo / LOCK).read_text(encoding="utf-8"))
    authority_paths = [FEATURE, PDF, LOCK]
    authority_paths.extend(
        path.relative_to(repo)
        for path in sorted((repo / SPEC_ROOT).rglob("*"))
        if path.is_file()
    )
    authorities = {
        str(path): digest_file(repo / path) for path in sorted(set(authority_paths))
    }
    dirty = dirty_entries(repo)
    excluded = [
        {"status": status, "path": path}
        for status, path in dirty if is_excluded(path)
    ]
    in_scope = [
        {"status": status, "path": path, "sha256": digest_file(repo / path)}
        for status, path in dirty
        if not is_excluded(path) and (repo / path).is_file()
    ]
    tracked_tree = run(repo, "rev-parse", "HEAD^{tree}")
    return {
        "schemaVersion": MANIFEST_SCHEMA,
        "feature": str(SPEC_ROOT),
        "authority": authorities,
        "source": {
            "head": run(repo, "rev-parse", "HEAD"),
            "tree": tracked_tree,
            "trackedFileCount": len(tracked_paths(repo)),
            "dirtyInScope": in_scope,
            "dirtyExcluded": excluded,
        },
        "dependencies": dependency_records(repo, lock),
        "toolchain": {
            "compiler": command("c++", "--version").splitlines()[0],
            "linker": command("ld", "--version").splitlines()[0],
            "python": command("python3", "--version"),
            "boostPkgConfig": command("pkg-config", "--modversion", "boost"),
            "ortPythonLocked": lock.get("pythonPackages", {}).get("onnxruntime-gpu", "UNAVAILABLE"),
            "ortCppLocked": lock.get("onnxRuntimeCpp", {}).get("version", "UNAVAILABLE"),
        },
        "lockDigest": digest_file(repo / LOCK),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    output = args.output if args.output.is_absolute() else repo / args.output
    body = build(repo)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(body, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(digest_file(output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
