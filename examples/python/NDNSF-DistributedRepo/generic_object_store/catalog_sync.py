#!/usr/bin/env python3
"""Retired Python catalog and repair sidecar."""

from __future__ import annotations

import sys


def main() -> int:
    print(
        "SPEC192_COMPATIBILITY_V1: catalog and repair state now belong to the "
        "C++ Persistent Repo service; a Python sidecar is not supported.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
