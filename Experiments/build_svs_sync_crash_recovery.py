#!/usr/bin/env python3
"""Build isolated sanitizer subjects for Spec 134.

The script never moves the active NDN-SVS ref and never edits Spec 133
worktrees. Baseline sanitizer subjects are detached worktrees at the exact
Boost-1.71-only clean head frozen by Spec 133.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
SVS_REPO = Path(os.environ.get("NDN_SVS_SOURCE_REPO", "/home/tianxing/NDN/ndn-svs"))
DEFAULT_OUTPUT = REPO / "build/spec134"
DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-crash-recovery.cpp"
IO_DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-io-qualification.cpp"
BASE_COMMIT = "a9944019f76791773604999f00128057b9534ace"
BASE_TREE = "945a321d473f44f29e8349a83ce60373f3e37420"
CLEAN_HEAD = "bf1e3e37f0c4c7a5a04d678f0fa439283ee46d2d"
CLEAN_TREE = "5fec9ee7aad4a124e34589d1fc5c5531bd4052ea"
BOOST_PATCH_SHA256 = "36c8d5429b0033c1350caebd6ab4fc1eacd0c35477845fec9644c083b6682307"
SCHEMA = "spec134-subject-manifest-v1"
REPAIR_SCHEMA = "spec134-repair-manifest-v1"
IO_SCHEMA = "spec134-io-qualification-manifest-v1"
MODES = {
    "asan-ubsan": "-std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined",
    "tsan": "-std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=thread",
}
MODE_COMPILERS = {"asan-ubsan": "g++", "tsan": "clang++"}
NORMAL_FLAGS = "-std=c++17 -O2 -g1"


def run(args: list[str], *, cwd: Path | None = None,
        env: dict[str, str] | None = None, check: bool = True) -> str:
    result = subprocess.run(
        args, cwd=cwd, env=env, text=True, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed rc={result.returncode}: {shlex.join(args)}\n{result.stdout}"
        )
    return result.stdout


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def git_value(cwd: Path, expression: str) -> str:
    return run(["git", "rev-parse", expression], cwd=cwd).strip()


def git_status(cwd: Path) -> str:
    return run(["git", "status", "--porcelain=v1", "--untracked-files=all"],
               cwd=cwd).strip()


def verify_source_identity() -> None:
    expected = {
        f"{BASE_COMMIT}^{{commit}}": BASE_COMMIT,
        f"{BASE_COMMIT}^{{tree}}": BASE_TREE,
        f"{CLEAN_HEAD}^{{commit}}": CLEAN_HEAD,
        f"{CLEAN_HEAD}^{{tree}}": CLEAN_TREE,
        f"{CLEAN_HEAD}^": BASE_COMMIT,
    }
    for expression, value in expected.items():
        actual = git_value(SVS_REPO, expression)
        if actual != value:
            raise RuntimeError(f"source identity mismatch {expression}: {actual}")
    paths = run(["git", "diff", "--name-only", f"{BASE_COMMIT}..{CLEAN_HEAD}"],
                cwd=SVS_REPO).splitlines()
    if paths != ["wscript"]:
        raise RuntimeError(f"clean head is not Boost-only: {paths}")
    diff = run(["git", "diff", f"{BASE_COMMIT}..{CLEAN_HEAD}", "--", "wscript"],
               cwd=SVS_REPO)
    for required in (
        "BOOST_VERSION_NUMBER < 107100",
        "minimum supported version of Boost is 1.71.0",
    ):
        if required not in diff:
            raise RuntimeError(f"canonical Boost edit missing: {required}")


def protected_snapshot(output: Path) -> dict[str, dict[str, str]]:
    owned = (output / "worktrees").resolve()
    records: dict[str, dict[str, str]] = {}
    current: dict[str, str] = {}
    text = run(["git", "worktree", "list", "--porcelain"], cwd=SVS_REPO)
    parsed: list[dict[str, str]] = []
    for line in text.splitlines() + [""]:
        if not line:
            if current:
                parsed.append(current)
                current = {}
            continue
        key, _, value = line.partition(" ")
        current[key] = value
    for record in parsed:
        path = Path(record["worktree"]).resolve()
        if path == owned or owned in path.parents:
            continue
        if path.is_dir():
            records[str(path)] = {
                "head": git_value(path, "HEAD"),
                "statusSha256": hashlib.sha256(
                    git_status(path).encode("utf-8")
                ).hexdigest(),
            }
        else:
            records[str(path)] = {"head": record.get("HEAD", "missing"),
                                  "statusSha256": "MISSING_WORKTREE_PATH"}
    return records


def ensure_worktree(output: Path, name: str, expected_head: str) -> Path:
    worktree = output / "worktrees" / name
    if not worktree.exists():
        worktree.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "worktree", "add", "--detach", str(worktree), expected_head],
            cwd=SVS_REPO)
    if git_value(worktree, "HEAD") != expected_head or git_status(worktree):
        raise RuntimeError(f"worktree identity/cleanliness failure: {name}")
    return worktree


def verify_boost_171(linkage: str) -> None:
    boost_rows = [line for line in linkage.splitlines() if "libboost_" in line]
    if not boost_rows or any(".1.71.0" not in line for line in boost_rows):
        raise RuntimeError(f"resolved Boost linkage is not exclusively 1.71:\n{linkage}")
    if "1.74" in linkage:
        raise RuntimeError("Boost 1.74 residue in resolved linkage")


def build_variant(output: Path, key: str, worktree_name: str, commit: str,
                  flags: str, compiler: str, *, driver: Path = DRIVER,
                  binary_prefix: str = "svs-sync-crash") -> dict[str, Any]:
    worktree = ensure_worktree(output, worktree_name, commit)
    env = dict(os.environ)
    env["CXX"] = compiler
    env["CXXFLAGS"] = flags
    env["LINKFLAGS"] = " ".join(
        flag for flag in shlex.split(flags) if not flag.startswith("-std=")
    )
    configure = [sys.executable, "waf", "configure", "--enable-shared", "--disable-static"]
    build = [sys.executable, "waf", "build", "-j2"]
    configure_log = output / f"{key}-configure.log"
    build_log = output / f"{key}-build.log"
    configure_text = run(configure, cwd=worktree, env=env)
    configure_log.write_text(configure_text, encoding="utf-8")
    build_text = run(build, cwd=worktree, env=env)
    build_log.write_text(build_text, encoding="utf-8")
    library = worktree / "build/libndn-svs.so"
    if not library.is_file():
        raise RuntimeError(f"missing sanitizer library: {library}")
    library_linkage = run(["ldd", str(library)])
    verify_boost_171(library_linkage)
    (output / f"{key}-library-ldd.txt").write_text(library_linkage,
                                                    encoding="utf-8")

    binary = output / "bin" / f"{binary_prefix}-{key}"
    binary.parent.mkdir(parents=True, exist_ok=True)
    pkg = shlex.split(run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]))
    compile_command = [
        compiler, *shlex.split(flags), "-pthread",
        "-I", str(worktree), "-I", str(worktree / "build"), str(driver),
        "-L", str(worktree / "build"), "-lndn-svs",
        f"-Wl,-rpath,{worktree / 'build'}", *pkg, *shlex.split(flags),
        "-o", str(binary),
    ]
    compile_text = run(compile_command)
    (output / f"{key}-driver-build.log").write_text(compile_text,
                                                     encoding="utf-8")
    binary_linkage = run(["ldd", str(binary)])
    verify_boost_171(binary_linkage)
    expected_library = str((worktree / "build/libndn-svs.so.0.1.0").resolve())
    if expected_library not in binary_linkage:
        raise RuntimeError(f"binary escaped isolated library: {key}")
    (output / f"{key}-binary-ldd.txt").write_text(binary_linkage,
                                                   encoding="utf-8")
    return {
        "mode": key,
        "compiler": run([compiler, "--version"]).splitlines()[0],
        "worktree": str(worktree.resolve()),
        "head": git_value(worktree, "HEAD"),
        "tree": git_value(worktree, "HEAD^{tree}"),
        "flags": flags,
        "configureCommand": configure,
        "buildCommand": build,
        "compileCommand": compile_command,
        "library": str(library.resolve()),
        "librarySha256": sha256_file(library),
        "libraryLdd": str((output / f"{key}-library-ldd.txt").resolve()),
        "binary": str(binary.resolve()),
        "binarySha256": sha256_file(binary),
        "binaryLdd": str((output / f"{key}-binary-ldd.txt").resolve()),
    }


def build_mode(output: Path, mode: str) -> dict[str, Any]:
    return build_variant(output, mode, f"baseline-{mode}", CLEAN_HEAD,
                         MODES[mode], MODE_COMPILERS[mode])


def build_diagnostics(output: Path) -> dict[str, Any]:
    if (output / "subject-manifest.json").exists():
        raise RuntimeError("Spec 134 subject manifest already exists; no in-place rebuild")
    output.mkdir(parents=True, exist_ok=True)
    verify_source_identity()
    before = protected_snapshot(output)
    subjects = {mode: build_mode(output, mode) for mode in MODES}
    after = protected_snapshot(output)
    if after != before:
        changed = [key for key in sorted(set(before) | set(after))
                   if before.get(key) != after.get(key)]
        raise RuntimeError(f"protected worktree changed: {changed}")
    manifest = {
        "schemaVersion": SCHEMA,
        "baseCommit": BASE_COMMIT,
        "baseTree": BASE_TREE,
        "cleanHead": CLEAN_HEAD,
        "cleanTree": CLEAN_TREE,
        "canonicalBoostPatchSha256": BOOST_PATCH_SHA256,
        "driver": str(DRIVER.resolve()),
        "driverSha256": sha256_file(DRIVER),
        "compiler": run(["g++", "--version"]).splitlines()[0],
        "boostVersionNumber": 107100,
        "publishApi": "publish",
        "parallelWorkers": None,
        "compressionEnabled": False,
        "subjects": subjects,
        "protectedWorktrees": after,
    }
    atomic_json(output / "subject-manifest.json", manifest)
    return manifest


def build_io_qualification(output: Path) -> dict[str, Any]:
    manifest_path = output / "io-qualification-manifest.json"
    if manifest_path.exists():
        raise RuntimeError(
            "Spec 134 I/O qualification manifest already exists; no in-place rebuild"
        )
    output.mkdir(parents=True, exist_ok=True)
    verify_source_identity()
    before = protected_snapshot(output)
    item = build_variant(
        output,
        "io-qualification-normal",
        "io-qualification-normal",
        CLEAN_HEAD,
        NORMAL_FLAGS,
        "g++",
        driver=IO_DRIVER,
        binary_prefix="svs-sync-io",
    )
    after = protected_snapshot(output)
    if after != before:
        changed = [
            key
            for key in sorted(set(before) | set(after))
            if before.get(key) != after.get(key)
        ]
        raise RuntimeError(f"protected worktree changed during I/O build: {changed}")
    manifest = {
        "schemaVersion": IO_SCHEMA,
        "baseCommit": BASE_COMMIT,
        "baseTree": BASE_TREE,
        "cleanHead": CLEAN_HEAD,
        "cleanTree": CLEAN_TREE,
        "canonicalBoostPatchSha256": BOOST_PATCH_SHA256,
        "driver": str(IO_DRIVER.resolve()),
        "driverSha256": sha256_file(IO_DRIVER),
        "compiler": run(["g++", "--version"]).splitlines()[0],
        "boostVersionNumber": 107100,
        "publishApi": "publish",
        "parallelWorkers": None,
        "compressionEnabled": False,
        "executionModel": "single-face-io-thread",
        "subjects": {"io-qualification-normal": item},
        "protectedWorktrees": after,
    }
    atomic_json(manifest_path, manifest)
    return manifest


def build_repair(output: Path, repair_worktree: Path) -> dict[str, Any]:
    repair_manifest = output / "repair-manifest.json"
    if repair_manifest.exists():
        raise RuntimeError("Spec 134 repair manifest already exists; no in-place rebuild")
    diagnostic_manifest = output / "subject-manifest.json"
    if not diagnostic_manifest.is_file():
        raise RuntimeError("diagnostic subject manifest is required")
    repair_worktree = repair_worktree.resolve()
    repair_head = git_value(repair_worktree, "HEAD")
    if git_value(repair_worktree, "HEAD^") != CLEAN_HEAD or git_status(repair_worktree):
        raise RuntimeError("repair worktree parent/cleanliness mismatch")
    repair_paths = run(["git", "diff", "--name-only", "HEAD^", "HEAD"],
                       cwd=repair_worktree).splitlines()
    expected_paths = ["ndn-svs/core.cpp", "ndn-svs/svspubsub.cpp"]
    if repair_paths != expected_paths:
        raise RuntimeError(f"repair changed unexpected paths: {repair_paths}")
    repair_patch = run(["git", "diff", "--binary", "HEAD^", "HEAD"],
                       cwd=repair_worktree).encode("utf-8")
    patch_path = output / "sync-crash-repair.patch"
    patch_path.write_bytes(repair_patch)

    before = protected_snapshot(output)
    variants = {}
    for mode in MODES:
        item = build_variant(output, f"repaired-{mode}", f"repaired-{mode}",
                             repair_head, MODES[mode], MODE_COMPILERS[mode])
        item["mode"] = mode
        variants[mode] = item
    variants["repaired-normal"] = build_variant(
        output, "repaired-normal", "repaired-normal", repair_head,
        NORMAL_FLAGS, "g++"
    )
    after = protected_snapshot(output)
    if after != before:
        changed = [key for key in sorted(set(before) | set(after))
                   if before.get(key) != after.get(key)]
        raise RuntimeError(f"protected worktree changed during repair build: {changed}")
    manifest = {
        "schemaVersion": REPAIR_SCHEMA,
        "diagnosticSubjectManifest": str(diagnostic_manifest.resolve()),
        "diagnosticSubjectManifestSha256": sha256_file(diagnostic_manifest),
        "baseCommit": BASE_COMMIT,
        "baseTree": BASE_TREE,
        "cleanHead": CLEAN_HEAD,
        "cleanTree": CLEAN_TREE,
        "canonicalBoostPatchSha256": BOOST_PATCH_SHA256,
        "repairHead": repair_head,
        "repairTree": git_value(repair_worktree, "HEAD^{tree}"),
        "repairParent": CLEAN_HEAD,
        "repairPaths": repair_paths,
        "repairPatch": str(patch_path.resolve()),
        "repairPatchSha256": sha256_file(patch_path),
        "driver": str(DRIVER.resolve()),
        "driverSha256": sha256_file(DRIVER),
        "publishApi": "publish",
        "parallelWorkers": None,
        "compressionEnabled": False,
        "subjects": variants,
        "protectedWorktrees": after,
    }
    atomic_json(repair_manifest, manifest)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("build-diagnostics")
    command.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    command.add_argument("--jobs", type=int, default=2)
    io_qualification = sub.add_parser("build-io-qualification")
    io_qualification.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    io_qualification.add_argument("--jobs", type=int, default=2)
    repair = sub.add_parser("build-repair")
    repair.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    repair.add_argument("--repair-worktree", type=Path,
                        default=DEFAULT_OUTPUT / "worktrees/repaired-subject")
    repair.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs != 2:
        raise RuntimeError("Spec 134 diagnostic builds are frozen at -j2")
    if args.command == "build-diagnostics":
        manifest = build_diagnostics(args.output.resolve())
    elif args.command == "build-io-qualification":
        manifest = build_io_qualification(args.output.resolve())
    else:
        manifest = build_repair(args.output.resolve(), args.repair_worktree)
    print(json.dumps(manifest, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
