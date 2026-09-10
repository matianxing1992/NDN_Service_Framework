#!/usr/bin/env python3
"""Fail when any installed ELF has an unresolved shared-library dependency."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess


ELF = b"\x7fELF"
# The NVIDIA driver ABI is supplied by ``apptainer exec --nv`` on iTiger.
# Every CUDA userspace DSO remains image-owned and fail closed.
HOST_DRIVER_LIBRARIES = {"libcuda.so.1"}


def library_search_dirs(path: Path) -> list[Path]:
    """Return the vendored directories needed to resolve *path* with ldd."""
    directories = [path.parent.resolve()]
    for ancestor in path.resolve().parents:
        try:
            children = list(ancestor.iterdir())
        except OSError:
            continue
        directories.extend(
            child.resolve()
            for child in children
            if child.is_dir() and child.name.endswith(".libs")
        )
    return list(dict.fromkeys(directories))


def _reject_prefixes(prefixes: tuple[str, ...]) -> tuple[str, ...]:
    normalized = []
    for prefix in prefixes:
        if not prefix.startswith("/") or any(char in prefix for char in "\x00\n\r"):
            raise RuntimeError(f"RUNTIME_REJECT_PREFIX_INVALID:{prefix}")
        normalized.append(prefix.rstrip("/") + "/")
    return tuple(dict.fromkeys(normalized))


def _dynamic_paths(path: Path) -> list[str]:
    result = subprocess.run(
        ["readelf", "-d", str(path)],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(f"RUNTIME_ELF_DYNAMIC_INFO_FAILED:{path}")
    paths: list[str] = []
    for line in result.stdout.splitlines():
        if "(RPATH)" not in line and "(RUNPATH)" not in line:
            continue
        match = re.search(r"\[(.*?)\]", line)
        if match:
            paths.extend(item for item in match.group(1).split(":") if item)
    return paths


def _resolved_paths(ldd_output: str) -> list[str]:
    paths: list[str] = []
    for line in ldd_output.splitlines():
        match = re.search(r"=>\s+(/[^\s(]+)", line)
        if match:
            paths.append(match.group(1))
            continue
        match = re.match(r"\s*(/[^\s(]+)", line)
        if match:
            paths.append(match.group(1))
    return paths


def verify_elf(path: Path, reject_prefixes: tuple[str, ...] = ()) -> None:
    reject_prefixes = _reject_prefixes(reject_prefixes)
    environment = dict(os.environ)
    search_path = os.pathsep.join(str(directory) for directory in library_search_dirs(path))
    inherited_library_path = environment.get("LD_LIBRARY_PATH")
    environment["LD_LIBRARY_PATH"] = (
        search_path
        if not inherited_library_path
        else os.pathsep.join((search_path, inherited_library_path))
    )
    result = subprocess.run(
        ["ldd", str(path)],
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode and "not a dynamic executable" not in (
        result.stdout + result.stderr
    ):
        raise RuntimeError(f"RUNTIME_LDD_FAILED:{path}")
    unresolved = set(
        re.findall(r"^\s*(\S+)\s+=>\s+not found\s*$", result.stdout, re.MULTILINE)
    )
    if unresolved - HOST_DRIVER_LIBRARIES:
        raise RuntimeError(f"RUNTIME_LIBRARY_MISSING:{path}")
    if reject_prefixes:
        observed = _dynamic_paths(path) + _resolved_paths(result.stdout)
        for observed_path in observed:
            for prefix in reject_prefixes:
                if observed_path.startswith(prefix):
                    raise RuntimeError(
                        f"RUNTIME_HOST_BOUND_PATH:{path}:{observed_path}:{prefix}"
                    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", action="append", required=True)
    parser.add_argument(
        "--reject-prefix", action="append", default=[],
        help="Fail when an ELF RUNPATH or resolved dependency starts with this absolute prefix.",
    )
    args = parser.parse_args()
    reject_prefixes = tuple(args.reject_prefix)
    checked = 0
    missing: list[str] = []
    for root in map(Path, args.root):
        for path in root.rglob("*"):
            try:
                is_elf = path.is_file() and path.open("rb").read(4) == ELF
            except OSError:
                continue
            if not is_elf:
                continue
            try:
                verify_elf(path, reject_prefixes)
            except RuntimeError as error:
                missing.append(str(error))
            checked += 1
    if missing:
        raise SystemExit("RUNTIME_LIBRARY_CLOSURE_INCOMPLETE\n" + "\n".join(missing))
    if checked == 0:
        raise SystemExit("RUNTIME_LIBRARY_CLOSURE_EMPTY")
    print(f"RUNTIME_LIBRARY_CLOSURE_PASS:{checked}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
