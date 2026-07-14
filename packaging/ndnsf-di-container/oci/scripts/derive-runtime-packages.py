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
    sibling_directory = str(path.parent.resolve())
    inherited_library_path = environment.get("LD_LIBRARY_PATH")
    environment["LD_LIBRARY_PATH"] = (
        sibling_directory
        if not inherited_library_path
        else os.pathsep.join((sibling_directory, inherited_library_path))
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
    if "not found" in result.stdout:
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
