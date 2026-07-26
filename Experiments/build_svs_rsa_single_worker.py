#!/usr/bin/env python3
"""Build and verify the single Spec 136 benchmark subject without replacing it."""

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
from typing import Any, Optional


REPO = Path(__file__).resolve().parents[1]
SVS_REPO = Path(
    os.environ.get("NDN_SVS_SOURCE_REPO", "/home/tianxing/NDN/ndn-svs")
).resolve()
DEFAULT_OUTPUT = REPO / "build/spec136-rsa-single-worker-r6"
BENCHMARK = (
    REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-rsa-single-worker.cpp"
)
RUNNER = REPO / "Experiments/NDN_SVS_RSA_Single_Worker_Minindn.py"
ANALYZER = REPO / "Experiments/analyze_svs_rsa_single_worker.py"


def run(command: list[str], *, cwd: Path = REPO) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return completed.stdout


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def repo_snapshot(path: Path) -> dict[str, Any]:
    status = run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], cwd=path
    )
    diff = run(["git", "diff", "--binary"], cwd=path).encode("utf-8")
    return {
        "path": str(path),
        "head": run(["git", "rev-parse", "HEAD"], cwd=path).strip(),
        "tree": run(["git", "rev-parse", "HEAD^{tree}"], cwd=path).strip(),
        "statusSha256": sha256_bytes(status.encode("utf-8")),
        "diffSha256": sha256_bytes(diff),
        "changedPaths": [
            row[3:] for row in status.splitlines() if len(row) >= 4
        ],
    }


def source_record(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise RuntimeError(f"source is missing: {path}")
    return {"path": str(path.resolve()), "sha256": sha256_file(path)}


def normalized_linkage(binary: Path) -> str:
    return re.sub(r"0x[0-9a-fA-F]+", "0xADDR", run(["ldd", str(binary)]))


def build(
    output: Path,
    library_dir: Path,
    *,
    manifest_schema: str = "spec136.build-manifest.v1",
    binary_name: str = "svs-rsa-single-worker",
    runner: Path = RUNNER,
    analyzer: Optional[Path] = ANALYZER,
    builder: Optional[Path] = None,
) -> Path:
    output = output.resolve()
    library_dir = library_dir.resolve()
    library = library_dir / "libndn-svs.so"
    manifest = output / "build-manifest.json"
    if manifest.exists():
        raise RuntimeError(f"refusing to overwrite frozen build: {manifest}")
    if not library.is_file():
        raise RuntimeError(f"NDN-SVS library is missing: {library}")
    output.mkdir(parents=True, exist_ok=True)

    sources = {
        "benchmark": source_record(BENCHMARK),
        "runner": source_record(runner),
        "builder": source_record(builder or Path(__file__).resolve()),
    }
    if analyzer is not None:
        sources["analyzer"] = source_record(analyzer)
    pkg_flags = shlex.split(
        run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]).strip()
    )
    binary = output / binary_name
    temporary = output / f"{binary_name}.tmp"
    command = [
        "g++",
        "-std=c++17",
        "-O2",
        "-g",
        "-pthread",
        "-I",
        str(SVS_REPO),
        "-I",
        str(SVS_REPO / "build"),
        str(BENCHMARK),
        "-L",
        str(library_dir),
        "-lndn-svs",
        f"-Wl,-rpath,{library_dir}",
        *pkg_flags,
        "-o",
        str(temporary),
    ]
    build_log = output / "benchmark-build.log"
    with build_log.open("w", encoding="utf-8") as stream:
        subprocess.run(
            command,
            cwd=REPO,
            check=True,
            text=True,
            stdout=stream,
            stderr=subprocess.STDOUT,
        )
    temporary.replace(binary)

    linkage = normalized_linkage(binary)
    if "1.74" in linkage:
        raise RuntimeError("Boost 1.74 residue detected in benchmark linkage")
    if "libboost_" in linkage and "1.71" not in linkage:
        raise RuntimeError("Boost 1.71 linkage was not established")
    linkage_path = output / "ldd.txt"
    linkage_path.write_text(linkage, encoding="utf-8")
    compiler = run(["g++", "--version"]).splitlines()[0]
    build_id_text = run(["readelf", "-n", str(binary)])
    build_id = "unavailable"
    for line in build_id_text.splitlines():
        if "Build ID:" in line:
            build_id = line.split("Build ID:", 1)[1].strip()
            break

    record = {
        "schema": manifest_schema,
        "createdUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "binary": str(binary),
        "binarySha256": sha256_file(binary),
        "binaryBuildId": build_id,
        "library": str(library),
        "librarySha256": sha256_file(library),
        "compiler": compiler,
        "compileCommand": command[:-1] + [str(binary)],
        "pkgConfigFlags": pkg_flags,
        "buildLog": source_record(build_log),
        "linkage": source_record(linkage_path),
        "sources": sources,
        "serviceFramework": repo_snapshot(REPO),
        "ndnSvs": repo_snapshot(SVS_REPO),
        "hostCpuCount": os.cpu_count(),
        "effectiveCpuAffinity": sorted(os.sched_getaffinity(0)),
    }
    write_json(manifest, record)
    print(manifest)
    return manifest


def verify(manifest: Path) -> None:
    record = json.loads(manifest.read_text(encoding="utf-8"))
    if record.get("schema") != "spec136.build-manifest.v1":
        raise RuntimeError("unexpected build manifest schema")
    checks = [
        (Path(record["binary"]), record["binarySha256"], "binary"),
        (Path(record["library"]), record["librarySha256"], "library"),
        (
            Path(record["buildLog"]["path"]),
            record["buildLog"]["sha256"],
            "build log",
        ),
        (
            Path(record["linkage"]["path"]),
            record["linkage"]["sha256"],
            "linkage",
        ),
    ]
    for source in record["sources"].values():
        checks.append((Path(source["path"]), source["sha256"], "source"))
    for path, expected, label in checks:
        if not path.is_file() or sha256_file(path) != expected:
            raise RuntimeError(f"{label} identity changed: {path}")
    linkage = normalized_linkage(Path(record["binary"]))
    if sha256_bytes(linkage.encode("utf-8")) != sha256_file(
        Path(record["linkage"]["path"])
    ):
        raise RuntimeError("resolved linkage changed")
    print(f"SPEC136_BUILD_VERIFY_OK {manifest}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("build", "verify"))
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--library-dir", type=Path, default=SVS_REPO / "build"
    )
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    if args.command == "build":
        build(args.output, args.library_dir)
    else:
        verify(
            args.manifest.resolve()
            if args.manifest
            else (args.output / "build-manifest.json").resolve()
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
