#!/usr/bin/env python3
"""Fail when any installed ELF has an unresolved shared-library dependency."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", action="append", required=True)
    args = parser.parse_args()
    checked = 0
    missing = []
    for root in map(Path, args.root):
        for path in root.rglob("*"):
            try:
                is_elf = path.is_file() and path.open("rb").read(4) == b"\x7fELF"
            except OSError:
                continue
            if not is_elf:
                continue
            result = subprocess.run(["ldd", str(path)], text=True, capture_output=True, check=False)
            if "not found" in result.stdout:
                missing.extend(f"{path}:{line.strip()}" for line in result.stdout.splitlines() if "not found" in line)
            checked += 1
    if missing:
        raise SystemExit("RUNTIME_LIBRARY_CLOSURE_INCOMPLETE\n" + "\n".join(missing))
    if checked == 0:
        raise SystemExit("RUNTIME_LIBRARY_CLOSURE_EMPTY")
    print(f"RUNTIME_LIBRARY_CLOSURE_PASS:{checked}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
