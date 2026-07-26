#!/usr/bin/env python3
"""Build the two immutable Spec 132 NDN-SVS capability subjects."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
SVS_REPO = Path("/home/tianxing/NDN/ndn-svs")
BUILD_ROOT = REPO / "build/spec132"
DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-pubsub-bench.cpp"
SUBJECTS = (
    ("sync-publish-no-internal-parallelism", "a9944019f76791773604999f00128057b9534ace", 0),
    ("async-publish-parallel-sync", "6bb34545b4f89f1f6c265a68c18f1a40ade413eb", 1),
)
OLD_GUARD = "BOOST_VERSION_NUMBER < 107400"
NEW_GUARD = "BOOST_VERSION_NUMBER < 107100"
OLD_MESSAGE = "minimum supported version of Boost is 1.74.0"
NEW_MESSAGE = "minimum supported version of Boost is 1.71.0"
PATCH_SCHEMA = "spec132-boost171-build-patch-v1"


def run(args: list[str], *, cwd: Path | None = None, capture: bool = True) -> str:
    result = subprocess.run(args, cwd=cwd, text=True, check=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.STDOUT if capture else None)
    return result.stdout if capture else ""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_patch_bytes() -> bytes:
    return (f"{PATCH_SCHEMA}\n"
            f"wscript\n{OLD_GUARD}\n{NEW_GUARD}\n"
            f"{OLD_MESSAGE}\n{NEW_MESSAGE}\n").encode()


def verify_base(commit: str) -> None:
    actual = run(["git", "rev-parse", f"{commit}^{{commit}}"], cwd=SVS_REPO).strip()
    if actual != commit:
        raise RuntimeError(f"subject commit did not resolve exactly: {commit} -> {actual}")


def ensure_worktree(subject: str, commit: str, root: Path) -> Path:
    worktree = root / "worktrees" / subject
    branch = f"spec132-build-{subject}"
    if worktree.exists():
        head = run(["git", "rev-parse", "HEAD"], cwd=worktree).strip()
        base = run(["git", "rev-parse", "HEAD^"], cwd=worktree).strip()
        if base != commit or head == commit:
            raise RuntimeError(f"existing worktree has wrong identity: {worktree}")
        if run(["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=worktree).strip():
            raise RuntimeError(f"existing build worktree is dirty: {worktree}")
        return worktree
    worktree.parent.mkdir(parents=True, exist_ok=True)
    branch_exists = subprocess.run(["git", "show-ref", "--verify", "--quiet",
                                    f"refs/heads/{branch}"], cwd=SVS_REPO).returncode == 0
    if branch_exists:
        raise RuntimeError(f"temporary branch exists without its worktree: {branch}")
    run(["git", "worktree", "add", "-b", branch, str(worktree), commit], cwd=SVS_REPO,
        capture=False)
    wscript = worktree / "wscript"
    original = wscript.read_text(encoding="utf-8")
    if original.count(OLD_GUARD) != 1 or original.count(OLD_MESSAGE) != 1:
        raise RuntimeError(f"unexpected pre-patch Boost guard in {subject}")
    changed = original.replace(OLD_GUARD, NEW_GUARD).replace(OLD_MESSAGE, NEW_MESSAGE)
    wscript.write_text(changed, encoding="utf-8")
    paths = run(["git", "diff", "--name-only"], cwd=worktree).splitlines()
    if paths != ["wscript"]:
        raise RuntimeError(f"build patch changed unexpected paths: {paths}")
    run(["git", "add", "wscript"], cwd=worktree)
    run(["git", "-c", "user.name=Spec132 Build", "-c",
         "user.email=spec132@invalid", "commit", "-m",
         "build: allow host Boost 1.71 for Spec 132"], cwd=worktree)
    return worktree


def build_subject(subject: str, commit: str, latest: int, root: Path) -> dict[str, Any]:
    verify_base(commit)
    worktree = ensure_worktree(subject, commit, root)
    head = run(["git", "rev-parse", "HEAD"], cwd=worktree).strip()
    tree = run(["git", "rev-parse", "HEAD^{tree}"], cwd=worktree).strip()
    parent = run(["git", "rev-parse", "HEAD^"], cwd=worktree).strip()
    status = run(["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=worktree).strip()
    if parent != commit or status:
        raise RuntimeError(f"invalid clean build identity for {subject}")
    diff = run(["git", "diff", "HEAD^", "HEAD", "--", "wscript"], cwd=worktree)
    if OLD_GUARD not in diff or NEW_GUARD not in diff or OLD_MESSAGE not in diff or NEW_MESSAGE not in diff:
        raise RuntimeError(f"canonical Boost patch not found for {subject}")
    if run(["git", "diff", "--name-only", "HEAD^", "HEAD"], cwd=worktree).splitlines() != ["wscript"]:
        raise RuntimeError(f"non-wscript build patch for {subject}")

    configure_log = root / f"{subject}-configure.log"
    build_log = root / f"{subject}-build.log"
    with configure_log.open("w", encoding="utf-8") as output:
        subprocess.run([sys.executable, "waf", "configure"], cwd=worktree, text=True,
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    with build_log.open("w", encoding="utf-8") as output:
        subprocess.run([sys.executable, "waf", "build", "-j2"], cwd=worktree,
                       text=True, stdout=output, stderr=subprocess.STDOUT, check=True)

    binary_dir = root / "bin" / subject
    binary_dir.mkdir(parents=True, exist_ok=True)
    binary = binary_dir / "svs-pubsub-bench"
    pkg = run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]).split()
    command = (["g++", "-std=c++17", "-O2", "-pthread", f"-DSPEC132_LATEST={latest}",
                "-I", str(worktree), "-I", str(worktree / "build"), str(DRIVER),
                "-L", str(worktree / "build"),
                f"-Wl,-rpath,{worktree / 'build'}", "-lndn-svs"] + pkg +
               ["-lssl", "-lcrypto", "-o", str(binary)])
    compile_log = root / f"{subject}-peer-build.log"
    with compile_log.open("w", encoding="utf-8") as output:
        subprocess.run(command, cwd=REPO, text=True, stdout=output,
                       stderr=subprocess.STDOUT, check=True)
    ldd = run(["ldd", str(binary)])
    (root / f"{subject}-ldd.txt").write_text(ldd, encoding="utf-8")
    lowered = ldd.lower()
    if "ndnsf" in lowered or "boost_" in lowered and ("1.74" in lowered or "1.74.0" in lowered):
        raise RuntimeError(f"forbidden runtime dependency for {subject}")
    self_test = run([str(binary), "--self-test"]).strip()
    if self_test != f"SPEC132_SELF_TEST_OK subject={subject}":
        raise RuntimeError(f"self-test failed for {subject}: {self_test}")
    library = worktree / "build/libndn-svs.so"
    return {
        "subject": subject, "baseCommit": commit, "temporaryHead": head,
        "temporaryTree": tree, "worktree": str(worktree),
        "library": str(library), "librarySha256": sha256_file(library),
        "binary": str(binary), "binarySha256": sha256_file(binary),
        "driverSha256": sha256_file(DRIVER), "lddSha256": sha256_file(root / f"{subject}-ldd.txt"),
        "selfTest": self_test, "compileCommand": command,
        "boostHeaderVersion": "1.71", "parallelWorkers": 4 if latest else None,
        "publishApi": "publishAsync" if latest else "publish",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=BUILD_ROOT)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    patch = canonical_patch_bytes()
    (root / "boost171-build.patch.identity").write_bytes(patch)
    active_before = run(["git", "symbolic-ref", "-q", "HEAD"], cwd=SVS_REPO).strip()
    head_before = run(["git", "rev-parse", "HEAD"], cwd=SVS_REPO).strip()
    records = [build_subject(*subject, root) for subject in SUBJECTS]
    if run(["git", "symbolic-ref", "-q", "HEAD"], cwd=SVS_REPO).strip() != active_before or \
       run(["git", "rev-parse", "HEAD"], cwd=SVS_REPO).strip() != head_before:
        raise RuntimeError("active NDN-SVS ref moved during build")
    manifest = {
        "schemaVersion": "spec132-subjects-v1",
        "canonicalPatchSha256": hashlib.sha256(patch).hexdigest(),
        "sourceRepository": str(SVS_REPO), "activeRefUnchanged": active_before,
        "activeHeadUnchanged": head_before, "compiler": run(["g++", "--version"]).splitlines()[0],
        "pkgConfig": run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]).strip(),
        "subjects": records,
    }
    target = root / "subjects.json"
    target.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
