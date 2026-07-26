#!/usr/bin/env python3
"""Build and freeze the single Spec 137 NDN-SVS experiment subject.

The builder never moves the active NDN-SVS checkout.  It admits one detached
worktree at the exact registered commit, the canonical Boost-1.71-only wscript
edit, and the reviewed common measurement patch.  Both runtime treatments are
then compiled into one executable and bound into ``source-manifest.json``.
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
import time
from typing import Any, Iterable


REPO = Path(__file__).resolve().parents[1]
SVS_REPO = Path(
    os.environ.get("NDN_SVS_SOURCE_REPO", "/home/tianxing/NDN/ndn-svs")
).resolve()
DEFAULT_OUTPUT = REPO / "build/spec137-four-core"
BENCHMARK = (
    REPO
    / "Experiments/ndn-svs-pubsub-benchmark/svs-serial-production-offload.cpp"
)
MEASUREMENT_PATCH = (
    REPO / "Experiments/ndn-svs-pubsub-benchmark/spec137-measurement.patch"
)

BASE_COMMIT = "6bb34545b4f89f1f6c265a68c18f1a40ade413eb"
BASE_TREE = "cc110d89083d2c0d63cf74292f4dcd4fab8aa194"
SCHEMA = "spec137.subject.v1"
BOOST_PATCH_SCHEMA = "spec137.boost171-build-patch.v1"
OLD_GUARD = "BOOST_VERSION_NUMBER < 107400"
NEW_GUARD = "BOOST_VERSION_NUMBER < 107100"
OLD_MESSAGE = "minimum supported version of Boost is 1.74.0"
NEW_MESSAGE = "minimum supported version of Boost is 1.71.0"
MEASUREMENT_PATHS = {
    "ndn-svs/core.cpp",
    "ndn-svs/core.hpp",
    "ndn-svs/svspubsub.cpp",
    "tests/unit-tests/core.t.cpp",
    "tests/unit-tests/svspubsub.t.cpp",
}
WORKTREE_PATHS = MEASUREMENT_PATHS | {"wscript"}


def run(
    args: Iterable[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    capture: bool = True,
) -> str:
    command = [str(value) for value in args]
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        check=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    return completed.stdout if capture else ""


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_artifact(path: Path, expected: str, label: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"{label} is missing: {path}")
    actual = sha256_file(path)
    if actual != expected:
        raise RuntimeError(
            f"{label} changed: expected {expected}, got {actual}: {path}"
        )


def canonical_boost_patch_bytes() -> bytes:
    return (
        f"{BOOST_PATCH_SCHEMA}\n"
        f"wscript\n{OLD_GUARD}\n{NEW_GUARD}\n"
        f"{OLD_MESSAGE}\n{NEW_MESSAGE}\n"
    ).encode("utf-8")


def validate_measurement_patch(text: str) -> set[str]:
    paths = set(
        re.findall(r"^diff --git a/(\S+) b/\S+$", text, flags=re.MULTILINE)
    )
    if paths != MEASUREMENT_PATHS:
        raise RuntimeError(
            "unexpected measurement patch paths: "
            f"expected={sorted(MEASUREMENT_PATHS)} actual={sorted(paths)}"
        )
    if "diff --git a/wscript" in text:
        raise RuntimeError("unexpected measurement patch mutation of wscript")
    return paths


def git_value(cwd: Path, expression: str) -> str:
    return run(["git", "rev-parse", expression], cwd=cwd).strip()


def git_status(cwd: Path) -> str:
    return run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=cwd
    ).rstrip("\n")


def parse_worktrees() -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    current: dict[str, str] = {}
    for line in run(
        ["git", "worktree", "list", "--porcelain"], cwd=SVS_REPO
    ).splitlines():
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


def protected_snapshot(output: Path) -> dict[str, dict[str, str]]:
    owned = output.resolve()
    snapshot: dict[str, dict[str, str]] = {}
    for record in parse_worktrees():
        path = Path(record["worktree"]).resolve()
        if path == owned or owned in path.parents:
            continue
        status = git_status(path) if path.is_dir() else "MISSING"
        snapshot[str(path)] = {
            "head": git_value(path, "HEAD") if path.is_dir() else record.get("HEAD", ""),
            "branch": record.get("branch", "detached"),
            "statusSha256": sha256_bytes(status.encode("utf-8")),
        }
    return snapshot


def tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    if not root.exists():
        return "MISSING"
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        digest.update(str(path.relative_to(root)).encode("utf-8"))
        digest.update(b"\0")
        digest.update(bytes.fromhex(sha256_file(path)))
    return digest.hexdigest()


def protected_evidence_snapshot() -> dict[str, str]:
    return {
        str(path.relative_to(REPO)): tree_digest(path)
        for path in (
            REPO / "specs/133-svs-sync-stage-profiling",
            REPO / "specs/135-svs-fetcher-window-validation",
            REPO / "specs/136-svs-rsa-signing-offload",
        )
    }


def boost_version(boost_root: Path) -> tuple[int, str, Path]:
    candidates = (
        boost_root / "include/boost/version.hpp",
        boost_root / "boost/version.hpp",
    )
    header = next((candidate for candidate in candidates if candidate.is_file()), None)
    if header is None:
        raise RuntimeError(f"Boost version.hpp is missing under {boost_root}")
    text = header.read_text(encoding="utf-8", errors="replace")
    number = re.search(r"^\s*#\s*define\s+BOOST_VERSION\s+(\d+)", text, re.M)
    library = re.search(
        r'^\s*#\s*define\s+BOOST_LIB_VERSION\s+"([^"]+)"', text, re.M
    )
    if number is None or library is None:
        raise RuntimeError(f"cannot parse Boost version from {header}")
    if int(number.group(1)) != 107100:
        raise RuntimeError(
            f"Spec 137 requires Boost 1.71, got {number.group(1)} from {header}"
        )
    return int(number.group(1)), library.group(1), header


def ensure_worktree(output: Path) -> Path:
    worktree = output / "worktrees/serial-production-offload"
    if not worktree.exists():
        worktree.parent.mkdir(parents=True, exist_ok=True)
        run(
            ["git", "worktree", "add", "--detach", str(worktree), BASE_COMMIT],
            cwd=SVS_REPO,
            capture=False,
        )
        wscript = worktree / "wscript"
        source = wscript.read_text(encoding="utf-8")
        if source.count(OLD_GUARD) != 1 or source.count(OLD_MESSAGE) != 1:
            raise RuntimeError("historical Boost guard does not match contract")
        wscript.write_text(
            source.replace(OLD_GUARD, NEW_GUARD).replace(
                OLD_MESSAGE, NEW_MESSAGE
            ),
            encoding="utf-8",
        )
        run(["git", "apply", str(MEASUREMENT_PATCH)], cwd=worktree)
    return worktree


def validate_worktree(worktree: Path, patch_bytes: bytes) -> dict[str, Any]:
    if git_value(worktree, "HEAD") != BASE_COMMIT:
        raise RuntimeError("Spec 137 worktree moved from the registered base commit")
    if git_value(worktree, "HEAD^{tree}") != BASE_TREE:
        raise RuntimeError("Spec 137 base tree mismatch")
    status_rows = git_status(worktree).splitlines()
    changed = {row[3:] for row in status_rows if len(row) >= 4}
    if changed != WORKTREE_PATHS:
        raise RuntimeError(
            f"unexpected worktree paths: expected={sorted(WORKTREE_PATHS)} "
            f"actual={sorted(changed)}"
        )
    wscript = (worktree / "wscript").read_text(encoding="utf-8")
    if OLD_GUARD in wscript or OLD_MESSAGE in wscript:
        raise RuntimeError("Boost 1.74 compatibility guard remains")
    if wscript.count(NEW_GUARD) != 1 or wscript.count(NEW_MESSAGE) != 1:
        raise RuntimeError("Boost 1.71 compatibility patch is not canonical")
    measurement_diff = run(
        ["git", "diff", "--binary", "--", *sorted(MEASUREMENT_PATHS)],
        cwd=worktree,
    ).encode("utf-8")
    if measurement_diff != patch_bytes:
        raise RuntimeError("worktree measurement diff does not match reviewed patch")
    full_diff = run(["git", "diff", "--binary"], cwd=worktree).encode("utf-8")
    return {
        "changedPaths": sorted(changed),
        "measurementDiffSha256": sha256_bytes(measurement_diff),
        "fullDiffSha256": sha256_bytes(full_diff),
        "patchedTreeIdentity": sha256_bytes(
            BASE_TREE.encode("ascii") + b"\0" + full_diff
        ),
    }


def write_log(
    path: Path,
    command: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
) -> None:
    with path.open("w", encoding="utf-8") as output:
        subprocess.run(
            command,
            cwd=cwd,
            env=env,
            text=True,
            stdout=output,
            stderr=subprocess.STDOUT,
            check=True,
        )


def linkage_record(artifact: Path, output: Path, label: str) -> dict[str, str]:
    text = run(["ldd", str(artifact)])
    if re.search(r"boost[^\n]*1[._-]?74", text, re.I):
        raise RuntimeError(f"Boost 1.74 residue in {label} linkage")
    path = output / f"{label}-ldd.txt"
    path.write_text(text, encoding="utf-8")
    return {
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
    }


def elf_build_id(binary: Path) -> str:
    text = run(["readelf", "-n", str(binary)])
    match = re.search(r"Build ID:\s*([0-9a-f]+)", text)
    return match.group(1) if match else "unavailable"


def build_subject(base: str, boost_root: Path, output: Path) -> Path:
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "source-manifest.json"
    if manifest_path.exists():
        raise RuntimeError(f"refusing to overwrite frozen manifest: {manifest_path}")
    if base != BASE_COMMIT:
        raise RuntimeError(f"base must be exactly {BASE_COMMIT}")
    if git_value(SVS_REPO, f"{base}^{{commit}}") != BASE_COMMIT:
        raise RuntimeError("registered NDN-SVS commit cannot be resolved")
    if git_value(SVS_REPO, f"{base}^{{tree}}") != BASE_TREE:
        raise RuntimeError("registered NDN-SVS tree changed")
    if not BENCHMARK.is_file() or not MEASUREMENT_PATCH.is_file():
        raise RuntimeError("Spec 137 benchmark or measurement patch is missing")

    patch_bytes = MEASUREMENT_PATCH.read_bytes()
    validate_measurement_patch(patch_bytes.decode("utf-8"))
    boost_number, boost_library, boost_header = boost_version(boost_root.resolve())
    before_worktrees = protected_snapshot(output)
    before_evidence = protected_evidence_snapshot()
    worktree = ensure_worktree(output)
    patch_record = validate_worktree(worktree, patch_bytes)

    build_env = dict(os.environ)
    include = boost_header.parents[1]
    build_env["CXXFLAGS"] = " ".join(
        filter(None, (build_env.get("CXXFLAGS", ""), f"-I{include}"))
    )
    lib_candidates = [
        boost_root / "lib",
        boost_root / "lib64",
        Path("/usr/lib/x86_64-linux-gnu"),
    ]
    lib_dirs = [path.resolve() for path in lib_candidates if path.is_dir()]
    build_env["LDFLAGS"] = " ".join(
        filter(
            None,
            (
                build_env.get("LDFLAGS", ""),
                " ".join(f"-L{path}" for path in lib_dirs),
            ),
        )
    )
    configure = [
        sys.executable,
        "waf",
        "configure",
        "--enable-shared",
        "--disable-static",
        "--with-tests",
    ]
    build = [sys.executable, "waf", "build", "-j2"]
    write_log(output / "configure.log", configure, cwd=worktree, env=build_env)
    config = worktree / "build/config.hpp"
    if not config.is_file():
        raise RuntimeError("NDN-SVS config.hpp is missing after configure")
    write_log(output / "build.log", build, cwd=worktree, env=build_env)
    library = worktree / "build/libndn-svs.so"
    if not library.is_file():
        raise RuntimeError("NDN-SVS shared library is missing after build")

    binary_dir = output / "bin"
    binary_dir.mkdir(parents=True, exist_ok=True)
    binary = binary_dir / "svs-serial-production-offload"
    pkg = shlex.split(run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]))
    compile_command = [
        "g++",
        "-std=c++17",
        "-O2",
        "-g",
        "-pthread",
        "-I",
        str(worktree),
        "-I",
        str(worktree / "build"),
        str(BENCHMARK),
        "-L",
        str(worktree / "build"),
        "-lndn-svs",
        f"-Wl,-rpath,{worktree / 'build'}",
        *pkg,
        "-o",
        str(binary),
    ]
    write_log(
        output / "benchmark-build.log",
        compile_command,
        cwd=REPO,
        env=build_env,
    )

    self_tests: dict[str, dict[str, Any]] = {}
    for mode in ("face-serial", "worker-serial"):
        completed = subprocess.run(
            [str(binary), "--self-test", mode],
            cwd=REPO,
            env={
                **os.environ,
                "LD_LIBRARY_PATH": str(library.parent),
            },
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=True,
        )
        config_record = json.loads(completed.stdout)
        self_tests[mode] = config_record
    changed = {
        key
        for key in self_tests["face-serial"]
        if self_tests["face-serial"][key] != self_tests["worker-serial"][key]
    }
    expected_runtime_delta = {
        "production_mode",
        "parallel_sync_production",
        "production_workers",
        "production_queue_capacity",
        "sign_in_worker",
        "build_extra_in_worker",
        "worker_cpu_active",
    }
    if changed != expected_runtime_delta:
        raise RuntimeError(f"self-test runtime delta escaped contract: {changed}")

    linkage = {
        "binary": linkage_record(binary, output, "binary"),
        "ndnSvs": linkage_record(library, output, "ndn-svs"),
    }
    if protected_snapshot(output) != before_worktrees:
        raise RuntimeError("protected NDN-SVS worktree state changed during build")
    if protected_evidence_snapshot() != before_evidence:
        raise RuntimeError("Spec 133/135/136 protected evidence changed during build")

    boost_identity = canonical_boost_patch_bytes()
    boost_identity_path = output / "boost171-build.patch.identity"
    boost_identity_path.write_bytes(boost_identity)
    linkage_text = run(["ldd", str(binary)])
    (output / "linkage.txt").write_text(linkage_text, encoding="utf-8")
    manifest: dict[str, Any] = {
        "schema": SCHEMA,
        "createdUnixNs": time.time_ns(),
        "baseCommit": BASE_COMMIT,
        "baseTree": BASE_TREE,
        **patch_record,
        "sourceRepository": str(SVS_REPO),
        "worktree": str(worktree.resolve()),
        "measurementPatch": str(MEASUREMENT_PATCH.resolve()),
        "measurementPatchSha256": sha256_file(MEASUREMENT_PATCH),
        "measurementPatchPaths": sorted(MEASUREMENT_PATHS),
        "boostPatchSha256": sha256_bytes(boost_identity),
        "boostPatchIdentity": str(boost_identity_path.resolve()),
        "boost": {
            "versionNumber": boost_number,
            "libraryVersion": boost_library,
            "root": str(boost_root.resolve()),
            "header": str(boost_header.resolve()),
            "libraryPaths": [str(path) for path in lib_dirs],
        },
        "compiler": {
            "path": run(["which", "g++"]).strip(),
            "version": run(["g++", "--version"]).splitlines()[0],
            "flags": ["-std=c++17", "-O2", "-g", "-pthread"],
        },
        "configureCommand": configure,
        "buildCommand": build,
        "compileCommand": compile_command,
        "benchmarkSource": str(BENCHMARK.resolve()),
        "benchmarkSourceSha256": sha256_file(BENCHMARK),
        "builderSource": str(Path(__file__).resolve()),
        "builderSourceSha256": sha256_file(Path(__file__)),
        "binary": str(binary.resolve()),
        "binarySha256": sha256_file(binary),
        "library": str(library.resolve()),
        "librarySha256": sha256_file(library),
        "elfBuildId": elf_build_id(binary),
        "linkage": linkage,
        "selfTests": self_tests,
        "runtimeTreatmentFields": sorted(expected_runtime_delta),
        "protectedWorktrees": before_worktrees,
        "protectedEvidence": before_evidence,
        "buildLogs": {
            name: sha256_file(output / name)
            for name in ("configure.log", "build.log", "benchmark-build.log")
        },
    }
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    binary.chmod(0o555)
    return manifest_path


def load_subject(path: Path) -> dict[str, Any]:
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot load subject manifest: {error}") from error
    if record.get("schema") != SCHEMA:
        raise RuntimeError("subject manifest schema mismatch")
    if record.get("baseCommit") != BASE_COMMIT or record.get("baseTree") != BASE_TREE:
        raise RuntimeError("subject source identity mismatch")
    verify_artifact(Path(record["binary"]), record["binarySha256"], "binary")
    verify_artifact(Path(record["library"]), record["librarySha256"], "library")
    verify_artifact(
        Path(record["measurementPatch"]),
        record["measurementPatchSha256"],
        "measurement patch",
    )
    validate_measurement_patch(
        Path(record["measurementPatch"]).read_text(encoding="utf-8")
    )
    verify_artifact(
        Path(record["benchmarkSource"]),
        record["benchmarkSourceSha256"],
        "benchmark source",
    )
    verify_artifact(
        Path(record["builderSource"]),
        record["builderSourceSha256"],
        "builder source",
    )
    verify_artifact(
        Path(record["boostPatchIdentity"]),
        record["boostPatchSha256"],
        "Boost 1.71 patch identity",
    )
    if Path(record["boostPatchIdentity"]).read_bytes() != canonical_boost_patch_bytes():
        raise RuntimeError("Boost 1.71 patch identity bytes changed")
    for label, linkage in record.get("linkage", {}).items():
        verify_artifact(Path(linkage["path"]), linkage["sha256"], f"{label} linkage")
        text = Path(linkage["path"]).read_text(encoding="utf-8", errors="replace")
        if re.search(r"boost[^\n]*1[._-]?74", text, re.I):
            raise RuntimeError(f"Boost 1.74 residue in {label} linkage")
    for name, expected in record.get("buildLogs", {}).items():
        verify_artifact(path.parent / name, expected, f"build log {name}")
    if record.get("boost", {}).get("versionNumber") != 107100:
        raise RuntimeError("subject is not bound to Boost 1.71")
    if protected_evidence_snapshot() != record.get("protectedEvidence"):
        raise RuntimeError("Spec 133/135/136 protected evidence changed after build")
    if protected_snapshot(path.parent) != record.get("protectedWorktrees"):
        raise RuntimeError("protected NDN-SVS checkout/ref state changed after build")
    if Path(record["binary"]).stat().st_mode & 0o222:
        raise RuntimeError("frozen subject binary is writable")
    return record


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--boost-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    path = build_subject(args.base, args.boost_root, args.output)
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
