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


def verify_elf(path: Path) -> None:
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
    if result.returncode and "not a dynamic executable" not in (
        result.stdout + result.stderr
    ):
        raise RuntimeError(f"RUNTIME_LDD_FAILED:{path}")
    unresolved = set(
        re.findall(r"^\s*(\S+)\s+=>\s+not found\s*$", result.stdout, re.MULTILINE)
    )
    if unresolved - HOST_DRIVER_LIBRARIES:
        raise RuntimeError(f"RUNTIME_LIBRARY_MISSING:{path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", action="append", required=True)
    args = parser.parse_args()
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
                verify_elf(path)
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
