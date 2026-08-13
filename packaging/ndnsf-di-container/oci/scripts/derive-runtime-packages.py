#!/usr/bin/env python3
"""Derive Ubuntu runtime packages from the installed ELF dependency closure."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess


ELF = b"\x7fELF"
BASE_PACKAGES = {"ca-certificates", "libgomp1", "python3", "zlib1g"}
# ``apptainer exec --nv`` injects the host NVIDIA driver ABI. CUDA userspace
# libraries remain image-owned; only this driver SONAME may be unresolved while
# deriving the image's Debian package closure on a GPU-less builder.
HOST_DRIVER_LIBRARIES = {"libcuda.so.1"}


def library_search_dirs(path: Path) -> list[Path]:
    """Return the vendored directories needed to resolve *path* with ldd.

    Python manylinux wheels (including Pillow) keep native DSOs in a sibling
    ``<distribution>.libs`` directory rather than beside the importing
    extension.  The runtime image must account for those files too; relying
    only on ``path.parent`` makes the build pass for the binary but fail when
    the same wheel is loaded in the final image.
    """
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


def elf_files(roots: list[Path]):
    for root in roots:
        for path in root.rglob("*"):
            try:
                if path.is_file() and path.open("rb").read(4) == ELF:
                    yield path
            except OSError:
                continue


def linked_paths(path: Path) -> list[Path]:
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
    if result.returncode and "not a dynamic executable" not in result.stderr:
        raise RuntimeError(f"RUNTIME_LDD_FAILED:{path}")
    unresolved = set(
        re.findall(r"^\s*(\S+)\s+=>\s+not found\s*$", result.stdout, re.MULTILINE)
    )
    if unresolved - HOST_DRIVER_LIBRARIES:
        raise RuntimeError(f"RUNTIME_LIBRARY_MISSING:{path}")
    values = []
    for line in result.stdout.splitlines():
        match = re.search(r"(?:=>\s+)?(/[^ ]+)\s+\(", line)
        if match:
            values.append(Path(match.group(1)).resolve())
    return values


def owning_package(path: Path) -> str | None:
    result = subprocess.run(
        ["dpkg-query", "-S", str(path)], text=True, capture_output=True, check=False
    )
    if result.returncode:
        return None
    return result.stdout.split(": ", 1)[0].split(":", 1)[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", action="append", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    packages = set(BASE_PACKAGES)
    for binary in elf_files([Path(value) for value in args.root]):
        for library in linked_paths(binary):
            if str(library).startswith(("/opt/", "/usr/local/cuda/")):
                continue
            package = owning_package(library)
            if package:
                packages.add(package)
    output = Path(args.output)
    output.write_text("\n".join(sorted(packages)) + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
