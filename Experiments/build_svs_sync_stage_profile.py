#!/usr/bin/env python3
"""Prepare and finalize the immutable Spec 133 NDN-SVS profiling subject.

The ``prepare`` mode belongs to T002. It creates a Boost-1.71-compatible clean
historical worktree, builds only its library, creates the profiling worktree at
the same clean head, and writes ``subject-foundation.json``. The ``finalize``
mode belongs to T008: it freezes the reviewed diagnostics-only patch, rebuilds
the profiled library, compiles the same driver against both subjects, runs both
self-tests, and writes the hash-bound final manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
SVS_REPO = Path(os.environ.get("NDN_SVS_SOURCE_REPO", "/home/tianxing/NDN/ndn-svs"))
BUILD_ROOT = REPO / "build/spec133"
DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-stage-profile.cpp"

BASE_COMMIT = "a9944019f76791773604999f00128057b9534ace"
BASE_TREE = "945a321d473f44f29e8349a83ce60373f3e37420"
CLEAN_BRANCH = "spec133-build-clean-control"
PROFILE_BRANCH = "spec133-build-sync-stage-profile"
FOUNDATION_SCHEMA = "spec133-subject-foundation-v1"
FINAL_SCHEMA = "spec133-subject-manifest-v1"
IO_FINAL_SCHEMA = "spec133-subject-manifest-io-v2"

OLD_GUARD = "BOOST_VERSION_NUMBER < 107400"
NEW_GUARD = "BOOST_VERSION_NUMBER < 107100"
OLD_MESSAGE = "minimum supported version of Boost is 1.74.0"
NEW_MESSAGE = "minimum supported version of Boost is 1.71.0"
PATCH_SCHEMA = "spec132-boost171-build-patch-v1"

PROFILE_PATCH_ALLOWLIST = {
    "ndn-svs/profile.hpp",
    "ndn-svs/profile.cpp",
    "ndn-svs/svspubsub.cpp",
    "ndn-svs/svsync-base.cpp",
    "ndn-svs/core.cpp",
    "ndn-svs/mapping-provider.cpp",
    "ndn-svs/fetcher.hpp",
    "ndn-svs/fetcher.cpp",
    "ndn-svs/version-vector.cpp",
    "ndn-svs/store-memory.hpp",
    "wscript",
}


def run(args: list[str], *, cwd: Path | None = None, capture: bool = True) -> str:
    result = subprocess.run(
        args,
        cwd=cwd,
        text=True,
        check=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    return result.stdout if capture else ""


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_patch_bytes() -> bytes:
    """Return the byte-identical Spec 131/132 Boost compatibility identity."""
    return (
        f"{PATCH_SCHEMA}\n"
        f"wscript\n{OLD_GUARD}\n{NEW_GUARD}\n"
        f"{OLD_MESSAGE}\n{NEW_MESSAGE}\n"
    ).encode("utf-8")


def git_value(cwd: Path, expression: str) -> str:
    return run(["git", "rev-parse", expression], cwd=cwd).strip()


def git_status(cwd: Path) -> str:
    return run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=cwd
    ).strip()


def branch_exists(branch: str) -> bool:
    return (
        subprocess.run(
            ["git", "show-ref", "--verify", "--quiet", f"refs/heads/{branch}"],
            cwd=SVS_REPO,
        ).returncode
        == 0
    )


def verify_base() -> None:
    commit = git_value(SVS_REPO, f"{BASE_COMMIT}^{{commit}}")
    tree = git_value(SVS_REPO, f"{BASE_COMMIT}^{{tree}}")
    if commit != BASE_COMMIT:
        raise RuntimeError(f"base commit did not resolve exactly: {commit}")
    if tree != BASE_TREE:
        raise RuntimeError(f"base tree mismatch: expected {BASE_TREE}, got {tree}")


def parse_worktrees() -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    current: dict[str, str] = {}
    for line in run(["git", "worktree", "list", "--porcelain"], cwd=SVS_REPO).splitlines():
        if not line:
            if current:
                records.append(current)
                current = {}
            continue
        key, _, value = line.partition(" ")
        current[key] = value
    if current:
        records.append(current)
    return records


def protected_snapshot(spec_root: Path) -> dict[str, dict[str, str]]:
    owned = (spec_root / "worktrees").resolve()
    snapshot: dict[str, dict[str, str]] = {}
    for record in parse_worktrees():
        path = Path(record["worktree"]).resolve()
        if path == owned or owned in path.parents:
            continue
        if not path.is_dir():
            snapshot[str(path)] = {
                "head": record.get("HEAD", "missing"),
                "branch": record.get("branch", record.get("detached", "detached")),
                "statusSha256": "MISSING_WORKTREE_PATH",
            }
            continue
        status = git_status(path)
        snapshot[str(path)] = {
            "head": git_value(path, "HEAD"),
            "branch": record.get("branch", record.get("detached", "detached")),
            "statusSha256": sha256_bytes(status.encode("utf-8")),
        }
    return snapshot


def verify_protected_snapshot(before: dict[str, dict[str, str]], spec_root: Path) -> None:
    after = protected_snapshot(spec_root)
    if after != before:
        changed = sorted(set(before) | set(after))
        details = [path for path in changed if before.get(path) != after.get(path)]
        raise RuntimeError(f"protected NDN-SVS worktree changed: {details}")


def validate_boost_patch(worktree: Path) -> tuple[str, str, bytes]:
    head = git_value(worktree, "HEAD")
    parent = git_value(worktree, "HEAD^")
    if parent != BASE_COMMIT:
        raise RuntimeError(f"clean head parent mismatch: {parent}")
    paths = run(["git", "diff", "--name-only", "HEAD^", "HEAD"], cwd=worktree).splitlines()
    if paths != ["wscript"]:
        raise RuntimeError(f"Boost patch changed unexpected paths: {paths}")
    source = (worktree / "wscript").read_text(encoding="utf-8")
    if OLD_GUARD in source or OLD_MESSAGE in source:
        raise RuntimeError("old Boost 1.74 guard remains after compatibility patch")
    if source.count(NEW_GUARD) != 1 or source.count(NEW_MESSAGE) != 1:
        raise RuntimeError("canonical Boost 1.71 substitutions are not exact")
    diff = run(["git", "diff", "--binary", "HEAD^", "HEAD"], cwd=worktree).encode("utf-8")
    if not diff:
        raise RuntimeError("Boost compatibility diff is empty")
    return head, git_value(worktree, "HEAD^{tree}"), diff


def ensure_clean_worktree(root: Path) -> tuple[Path, str, str, bytes]:
    worktree = root / "worktrees/clean-control"
    if not worktree.exists():
        if branch_exists(CLEAN_BRANCH):
            raise RuntimeError(f"orphan temporary branch exists: {CLEAN_BRANCH}")
        worktree.parent.mkdir(parents=True, exist_ok=True)
        run(
            ["git", "worktree", "add", "-b", CLEAN_BRANCH, str(worktree), BASE_COMMIT],
            cwd=SVS_REPO,
            capture=False,
        )
        wscript = worktree / "wscript"
        original = wscript.read_text(encoding="utf-8")
        if original.count(OLD_GUARD) != 1 or original.count(OLD_MESSAGE) != 1:
            raise RuntimeError("unexpected historical Boost guard")
        changed = original.replace(OLD_GUARD, NEW_GUARD).replace(OLD_MESSAGE, NEW_MESSAGE)
        wscript.write_text(changed, encoding="utf-8")
        if run(["git", "diff", "--name-only"], cwd=worktree).splitlines() != ["wscript"]:
            raise RuntimeError("Boost compatibility edit escaped wscript")
        run(["git", "add", "wscript"], cwd=worktree)
        run(
            [
                "git", "-c", "user.name=Spec133 Build", "-c",
                "user.email=spec133@invalid", "commit", "-m",
                "build: allow host Boost 1.71 for Spec 133",
            ],
            cwd=worktree,
        )
    if git_status(worktree):
        raise RuntimeError(f"clean-control worktree is dirty: {worktree}")
    head, tree, diff = validate_boost_patch(worktree)
    return worktree, head, tree, diff


def ensure_profile_worktree(root: Path, clean_head: str) -> Path:
    worktree = root / "worktrees/sync-stage-profile"
    if not worktree.exists():
        if branch_exists(PROFILE_BRANCH):
            raise RuntimeError(f"orphan temporary branch exists: {PROFILE_BRANCH}")
        run(
            ["git", "worktree", "add", "-b", PROFILE_BRANCH, str(worktree), clean_head],
            cwd=SVS_REPO,
            capture=False,
        )
    if git_value(worktree, "HEAD") != clean_head:
        raise RuntimeError("profiling worktree does not start at the clean head")
    if git_status(worktree):
        raise RuntimeError("profiling worktree is dirty before instrumentation")
    return worktree


def verify_compression_disabled(config_text: str) -> None:
    if re.search(r"^\s*#\s*define\s+NDN_SVS_COMPRESSION\s+1\s*$", config_text, re.M):
        raise RuntimeError("compression is not disabled")
    disabled = (
        re.search(r"^\s*#\s*define\s+NDN_SVS_COMPRESSION\s+0\s*$", config_text, re.M)
        or re.search(r"^\s*/\*\s*#undef\s+NDN_SVS_COMPRESSION\s*\*/\s*$", config_text, re.M)
    )
    if not disabled:
        raise RuntimeError("compression state missing from generated config")


def compiler_boost_version() -> tuple[int, str]:
    macros = run(
        ["g++", "-E", "-dM", "-include", "boost/version.hpp", "-x", "c++", "/dev/null"]
    )
    number_match = re.search(r"^#define BOOST_VERSION\s+(\d+)$", macros, re.M)
    library_match = re.search(r'^#define BOOST_LIB_VERSION\s+"([^"]+)"$', macros, re.M)
    if number_match is None or library_match is None:
        raise RuntimeError("cannot resolve compiler Boost version")
    return int(number_match.group(1)), library_match.group(1)


def audit_no_internal_parallelism(worktree: Path) -> dict[str, list[str]]:
    queries = {
        "publishAsync": r"publishAsync",
        "parallelSync": r"setParallelSyncProcessing|ParallelSync|m_parallel",
        "internalThreads": r"std::thread|std::jthread|thread_pool",
    }
    matches: dict[str, list[str]] = {}
    for key, pattern in queries.items():
        result = subprocess.run(
            ["git", "grep", "-n", "-E", pattern, "HEAD", "--", "ndn-svs"],
            cwd=worktree,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if result.returncode not in (0, 1):
            raise RuntimeError(f"source audit failed for {key}: {result.stdout}")
        rows = [line for line in result.stdout.splitlines() if line]
        matches[key] = rows
        if rows:
            raise RuntimeError(f"historical subject contains forbidden {key}: {rows}")
    return matches


def build_clean_library(worktree: Path, root: Path) -> dict[str, Any]:
    configure_log = root / "clean-control-configure.log"
    build_log = root / "clean-control-build.log"
    configure_command = [sys.executable, "waf", "configure", "--enable-shared", "--disable-static"]
    build_command = [sys.executable, "waf", "build", "-j2"]
    with configure_log.open("w", encoding="utf-8") as output:
        subprocess.run(
            configure_command,
            cwd=worktree,
            text=True,
            stdout=output,
            stderr=subprocess.STDOUT,
            check=True,
        )
    config = worktree / "build/config.hpp"
    if not config.is_file():
        raise RuntimeError("NDN-SVS generated config.hpp is missing")
    verify_compression_disabled(config.read_text(encoding="utf-8"))
    with build_log.open("w", encoding="utf-8") as output:
        subprocess.run(
            build_command,
            cwd=worktree,
            text=True,
            stdout=output,
            stderr=subprocess.STDOUT,
            check=True,
        )
    library = worktree / "build/libndn-svs.so"
    if not library.is_file():
        raise RuntimeError(f"clean library missing: {library}")
    ldd_path = root / "clean-control-library-ldd.txt"
    ldd = run(["ldd", str(library)])
    ldd_path.write_text(ldd, encoding="utf-8")
    if re.search(r"boost[^\n]*1\.74", ldd, re.I):
        raise RuntimeError("Boost 1.74 residue in clean library linkage")
    boost_number, boost_library = compiler_boost_version()
    if boost_number != 107100:
        raise RuntimeError(f"expected compiler Boost 1.71, got {boost_number}")
    return {
        "cleanLibrary": str(library.resolve()),
        "cleanLibrarySha256": sha256_file(library),
        "cleanLibraryLdd": str(ldd_path.resolve()),
        "cleanLibraryLddSha256": sha256_file(ldd_path),
        "configureCommand": configure_command,
        "buildCommand": build_command,
        "configureLogSha256": sha256_file(configure_log),
        "buildLogSha256": sha256_file(build_log),
        "boostVersionNumber": boost_number,
        "boostLibVersion": boost_library,
        "compressionEnabled": False,
    }


def prepare(root: Path) -> Path:
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    foundation_path = root / "subject-foundation.json"
    if foundation_path.exists():
        raise RuntimeError(f"foundation already exists; refusing overwrite: {foundation_path}")
    verify_base()
    before = protected_snapshot(root)
    clean_worktree, clean_head, clean_tree, boost_diff = ensure_clean_worktree(root)
    source_audit = audit_no_internal_parallelism(clean_worktree)
    build = build_clean_library(clean_worktree, root)
    if git_status(clean_worktree):
        raise RuntimeError("clean-control worktree became dirty during build")
    profile_worktree = ensure_profile_worktree(root, clean_head)
    verify_protected_snapshot(before, root)
    patch_identity = canonical_patch_bytes()
    patch_path = root / "boost171-build.patch.identity"
    patch_path.write_bytes(patch_identity)
    manifest: dict[str, Any] = {
        "schemaVersion": FOUNDATION_SCHEMA,
        "baseCommit": BASE_COMMIT,
        "baseTree": BASE_TREE,
        "cleanBranch": CLEAN_BRANCH,
        "cleanHead": clean_head,
        "cleanTree": clean_tree,
        "cleanWorktree": str(clean_worktree.resolve()),
        "profileBranch": PROFILE_BRANCH,
        "profileWorktree": str(profile_worktree.resolve()),
        "profileStartHead": clean_head,
        "profileStartTree": clean_tree,
        "canonicalBoostPatchSha256": sha256_bytes(patch_identity),
        "canonicalBoostPatchPath": str(patch_path.resolve()),
        "boostGitDiffSha256": sha256_bytes(boost_diff),
        "sourceRepository": str(SVS_REPO.resolve()),
        "protectedWorktrees": before,
        "sourceAudit": source_audit,
        "publishApi": "publish",
        "parallelWorkers": None,
        "compiler": run(["g++", "--version"]).splitlines()[0],
        "pkgConfig": run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]).strip(),
        **build,
    }
    foundation_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return foundation_path


def load_foundation(path: Path, *, require_worktrees: bool = True) -> dict[str, Any]:
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot load subject foundation: {error}") from error
    if record.get("schemaVersion") != FOUNDATION_SCHEMA:
        raise RuntimeError("foundation schema mismatch")
    if record.get("baseCommit") != BASE_COMMIT:
        raise RuntimeError("foundation base commit mismatch")
    if record.get("baseTree") != BASE_TREE:
        raise RuntimeError("foundation base tree mismatch")
    if record.get("compressionEnabled") is not False:
        raise RuntimeError("foundation compression state is not disabled")
    library = Path(str(record.get("cleanLibrary", "")))
    if not library.is_file() or sha256_file(library) != record.get("cleanLibrarySha256"):
        raise RuntimeError("foundation clean library is missing or changed")
    if require_worktrees:
        for key in ("cleanWorktree", "profileWorktree"):
            if not Path(str(record.get(key, ""))).is_dir():
                raise RuntimeError(f"foundation {key} is missing")
    return record


def validate_profile_paths(paths: list[str]) -> None:
    if not paths:
        raise RuntimeError("profiling patch is empty")
    unexpected = sorted(set(paths) - PROFILE_PATCH_ALLOWLIST)
    if unexpected:
        raise RuntimeError(f"unexpected profiling patch paths: {unexpected}")


def changed_profile_paths(worktree: Path) -> list[str]:
    rows = run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=worktree
    ).splitlines()
    return sorted(row[3:] for row in rows if len(row) >= 4)


def compile_driver(subject_root: Path, output: Path, profiled: bool,
                   log_path: Path) -> list[str]:
    pkg = run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]).strip().split()
    build_dir = subject_root / "build"
    command = [
        "g++", "-std=c++17", "-O2", "-pthread",
        f"-DSPEC133_PROFILED={1 if profiled else 0}",
        "-I", str(subject_root), "-I", str(build_dir), str(DRIVER),
        "-L", str(build_dir), "-lndn-svs", f"-Wl,-rpath,{build_dir}",
        *pkg, "-o", str(output),
    ]
    with log_path.open("w", encoding="utf-8") as log:
        subprocess.run(command, text=True, stdout=log, stderr=subprocess.STDOUT, check=True)
    return command


def run_self_test(binary: Path, library: Path, profiled: bool, root: Path) -> Path:
    log_path = root / f"{binary.name}-self-test.log"
    environment = dict(os.environ)
    environment["LD_LIBRARY_PATH"] = str(library.parent)
    if profiled:
        environment.update({
            "NDN_LOG": "ndn_svs.Profile=TRACE",
            "NDN_SVS_PROFILE_ENABLED": "1",
            "NDN_SVS_PROFILE_CELL_ID": "finalize-self-test",
            "NDN_SVS_PROFILE_PEER_ID": "local",
            "NDN_SVS_PROFILE_SAMPLE_MODULUS": "1",
        })
    with log_path.open("w", encoding="utf-8") as log:
        subprocess.run([str(binary), "--self-test"], env=environment, text=True,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    text = log_path.read_text(encoding="utf-8")
    if "SPEC133_SELF_TEST_OK" not in text or "piggyback=1 fallback=1" not in text:
        raise RuntimeError(f"driver self-test evidence missing: {log_path}")
    if profiled and (text.count("event=profile-start") != 1 or
                     text.count("event=profile-stop") != 1 or
                     text.count("event=stage-summary") != 81):
        raise RuntimeError("profiled self-test schema/summary count mismatch")
    return log_path


def finalize(
    root: Path,
    *,
    foundation_path: Path | None = None,
    dry_contract: bool = False,
) -> Path:
    root = root.resolve()
    foundation_path = foundation_path or root / "subject-foundation.json"
    foundation = load_foundation(foundation_path, require_worktrees=not dry_contract)
    if not DRIVER.is_file():
        raise RuntimeError(f"shared Spec 133 driver is missing: {DRIVER}")
    profile_worktree = Path(foundation["profileWorktree"])
    if not profile_worktree.is_dir():
        raise RuntimeError("profiling worktree is missing")
    changed = changed_profile_paths(profile_worktree)
    validate_profile_paths(changed)
    if dry_contract:
        return root / "subject-manifest.json"

    manifest_path = root / "subject-manifest.json"
    if manifest_path.exists():
        raise RuntimeError(f"subject manifest already exists; refusing overwrite: {manifest_path}")
    if git_value(profile_worktree, "HEAD") != foundation["profileStartHead"]:
        raise RuntimeError("profiling worktree moved before freeze")
    before = protected_snapshot(root)

    # Stage only the reviewed allowlist, then save exactly the bytes committed.
    run(["git", "add", "--", *changed], cwd=profile_worktree)
    staged_paths = run(["git", "diff", "--cached", "--name-only"], cwd=profile_worktree).splitlines()
    validate_profile_paths(staged_paths)
    if staged_paths != changed:
        raise RuntimeError("staged profiling patch path set mismatch")
    patch = run(["git", "diff", "--cached", "--binary"], cwd=profile_worktree).encode("utf-8")
    if not patch:
        raise RuntimeError("reviewed profiling patch is empty")
    patch_path = root / "sync-stage-profile.patch"
    patch_path.write_bytes(patch)
    patch_sha256 = sha256_bytes(patch)
    run([
        "git", "-c", "user.name=Spec133 Build", "-c", "user.email=spec133@invalid",
        "commit", "-m", "diagnostics: add Spec 133 synchronous stage profiling",
    ], cwd=profile_worktree)
    if git_status(profile_worktree):
        raise RuntimeError("profiling worktree is dirty after freeze commit")
    profiled_head = git_value(profile_worktree, "HEAD")
    profiled_tree = git_value(profile_worktree, "HEAD^{tree}")
    if git_value(profile_worktree, "HEAD^") != foundation["cleanHead"]:
        raise RuntimeError("profiling commit parent is not the clean subject")
    committed_paths = run(["git", "diff", "--name-only", "HEAD^", "HEAD"], cwd=profile_worktree).splitlines()
    if committed_paths != staged_paths:
        raise RuntimeError("committed profiling path set mismatch")
    source_audit = audit_no_internal_parallelism(profile_worktree)

    configure_log = root / "profiled-configure.log"
    build_log = root / "profiled-build.log"
    configure_command = [sys.executable, "waf", "configure", "--enable-shared", "--disable-static"]
    build_command = [sys.executable, "waf", "build", "-j2"]
    with configure_log.open("w", encoding="utf-8") as output:
        subprocess.run(configure_command, cwd=profile_worktree, text=True,
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    verify_compression_disabled((profile_worktree / "build/config.hpp").read_text(encoding="utf-8"))
    with build_log.open("w", encoding="utf-8") as output:
        subprocess.run(build_command, cwd=profile_worktree, text=True,
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    profiled_library = profile_worktree / "build/libndn-svs.so"
    clean_library = Path(foundation["cleanLibrary"])
    if not profiled_library.is_file():
        raise RuntimeError("profiled library missing after build")

    binary_dir = root / "bin"
    binary_dir.mkdir(parents=True, exist_ok=True)
    clean_binary = binary_dir / "svs-sync-clean-control"
    profiled_binary = binary_dir / "svs-sync-stage-profile"
    clean_compile = compile_driver(Path(foundation["cleanWorktree"]), clean_binary, False,
                                   root / "clean-driver-build.log")
    profiled_compile = compile_driver(profile_worktree, profiled_binary, True,
                                      root / "profiled-driver-build.log")
    clean_test = run_self_test(clean_binary, clean_library, False, root)
    profiled_test = run_self_test(profiled_binary, profiled_library, True, root)

    linkage: dict[str, str] = {}
    for label, artifact in (("cleanBinary", clean_binary), ("profiledBinary", profiled_binary),
                            ("profiledLibrary", profiled_library)):
        text = run(["ldd", str(artifact)])
        path = root / f"{label}-ldd.txt"
        path.write_text(text, encoding="utf-8")
        if re.search(r"boost[^\n]*1\.74", text, re.I):
            raise RuntimeError(f"Boost 1.74 residue in {label}")
        linkage[label] = str(path.resolve())

    header = (profile_worktree / "ndn-svs/profile.hpp").read_text(encoding="utf-8")
    stage_count = len(re.findall(r'X\([A-Z0-9_]+, "[A-Z]+\.[A-Z0-9_.]+"', header))
    if stage_count != 81:
        raise RuntimeError(f"frozen stage count mismatch: {stage_count}")
    if "publishAsync" in DRIVER.read_text(encoding="utf-8"):
        raise RuntimeError("shared driver reaches asynchronous publication")
    verify_protected_snapshot(before, root)

    manifest: dict[str, Any] = {
        **foundation,
        "schemaVersion": FINAL_SCHEMA,
        "subjectId": "sync-publish-no-internal-parallelism-profiled",
        "profilePatch": str(patch_path.resolve()),
        "profilePatchSha256": patch_sha256,
        "profilePatchPaths": committed_paths,
        "profiledHead": profiled_head,
        "profiledTree": profiled_tree,
        "profiledLibrary": str(profiled_library.resolve()),
        "profiledLibrarySha256": sha256_file(profiled_library),
        "cleanBinary": str(clean_binary.resolve()),
        "cleanBinarySha256": sha256_file(clean_binary),
        "profiledBinary": str(profiled_binary.resolve()),
        "profiledBinarySha256": sha256_file(profiled_binary),
        "driver": str(DRIVER.resolve()),
        "driverSha256": sha256_file(DRIVER),
        "cleanCompileCommand": clean_compile,
        "profiledCompileCommand": profiled_compile,
        "profiledConfigureCommand": configure_command,
        "profiledBuildCommand": build_command,
        "profiledConfigureLogSha256": sha256_file(configure_log),
        "profiledBuildLogSha256": sha256_file(build_log),
        "selfTests": {"clean": str(clean_test.resolve()), "profiled": str(profiled_test.resolve())},
        "linkage": linkage,
        "sourceAudit": source_audit,
        "asyncSymbolsReachable": False,
        "publishApi": "publish",
        "parallelWorkers": None,
        "compressionEnabled": False,
        "securityProfile": {"syncInterest": "HMAC", "data": "SHA256", "validators": "disabled"},
        "profileConfig": {
            "logger": "ndn_svs.Profile=TRACE", "sampleModulus": 100,
            "stageCount": stage_count, "clock": "CLOCK_MONOTONIC_RAW",
            "spanSchema": "spec133-stage-span-v1",
            "summarySchema": "spec133-stage-summary-v1",
        },
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    return manifest_path


def refreeze_io(root: Path) -> Path:
    root = root.resolve()
    source_manifest_path = root / "subject-manifest.json"
    output_manifest_path = root / "subject-manifest-io.json"
    if output_manifest_path.exists():
        raise RuntimeError(
            f"I/O subject manifest already exists; refusing overwrite: {output_manifest_path}"
        )
    source = json.loads(source_manifest_path.read_text(encoding="utf-8"))
    if source.get("schemaVersion") != FINAL_SCHEMA:
        raise RuntimeError("frozen profiling subject manifest schema mismatch")
    for key in ("cleanLibrary", "profiledLibrary"):
        artifact = Path(source[key])
        if not artifact.is_file() or sha256_file(artifact) != source[f"{key}Sha256"]:
            raise RuntimeError(f"frozen profiling library drift: {key}")
    clean_worktree = Path(source["cleanWorktree"])
    profile_worktree = Path(source["profileWorktree"])
    if git_value(clean_worktree, "HEAD") != source["cleanHead"] or git_status(clean_worktree):
        raise RuntimeError("clean profiling worktree moved or became dirty")
    if git_value(profile_worktree, "HEAD") != source["profiledHead"] or git_status(profile_worktree):
        raise RuntimeError("profiled worktree moved or became dirty")
    before = protected_snapshot(root)

    binary_dir = root / "bin"
    binary_dir.mkdir(parents=True, exist_ok=True)
    clean_binary = binary_dir / "svs-sync-clean-control-io"
    profiled_binary = binary_dir / "svs-sync-stage-profile-io"
    for artifact in (clean_binary, profiled_binary):
        if artifact.exists():
            raise RuntimeError(f"I/O binary already exists; refusing overwrite: {artifact}")
    clean_compile = compile_driver(
        clean_worktree, clean_binary, False, root / "clean-io-driver-build.log"
    )
    profiled_compile = compile_driver(
        profile_worktree, profiled_binary, True, root / "profiled-io-driver-build.log"
    )
    clean_test = run_self_test(
        clean_binary, Path(source["cleanLibrary"]), False, root
    )
    profiled_test = run_self_test(
        profiled_binary, Path(source["profiledLibrary"]), True, root
    )

    linkage = dict(source.get("linkage", {}))
    for label, artifact in (
        ("cleanIoBinary", clean_binary),
        ("profiledIoBinary", profiled_binary),
    ):
        text = run(["ldd", str(artifact)])
        path = root / f"{label}-ldd.txt"
        path.write_text(text, encoding="utf-8")
        if re.search(r"boost[^\n]*1\.74", text, re.I):
            raise RuntimeError(f"Boost 1.74 residue in {label}")
        linkage[label] = str(path.resolve())

    driver_text = DRIVER.read_text(encoding="utf-8")
    for forbidden in ("std::thread", "publishAsync", "sleep_until"):
        if forbidden in driver_text:
            raise RuntimeError(f"single-I/O driver contains forbidden behavior: {forbidden}")
    for required in ("boost::asio::steady_timer", "--io-cpu",
                     "single-face-io-thread"):
        if required not in driver_text:
            raise RuntimeError(f"single-I/O driver contract missing: {required}")
    verify_protected_snapshot(before, root)

    manifest = {
        **source,
        "schemaVersion": IO_FINAL_SCHEMA,
        "sourceManifest": str(source_manifest_path.resolve()),
        "sourceManifestSha256": sha256_file(source_manifest_path),
        "executionModel": "single-face-io-thread",
        "cleanBinary": str(clean_binary.resolve()),
        "cleanBinarySha256": sha256_file(clean_binary),
        "profiledBinary": str(profiled_binary.resolve()),
        "profiledBinarySha256": sha256_file(profiled_binary),
        "driver": str(DRIVER.resolve()),
        "driverSha256": sha256_file(DRIVER),
        "cleanCompileCommand": clean_compile,
        "profiledCompileCommand": profiled_compile,
        "selfTests": {
            "clean": str(clean_test.resolve()),
            "profiled": str(profiled_test.resolve()),
        },
        "linkage": linkage,
    }
    output_manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return output_manifest_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("prepare", "finalize", "refreeze-io"))
    parser.add_argument("--output", type=Path, default=BUILD_ROOT)
    args = parser.parse_args(argv)
    if args.mode == "prepare":
        target = prepare(args.output)
    elif args.mode == "finalize":
        target = finalize(args.output)
    else:
        target = refreeze_io(args.output)
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
