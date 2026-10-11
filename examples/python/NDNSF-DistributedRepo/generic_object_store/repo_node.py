#!/usr/bin/env python3
"""Retired Python Repo server launcher."""

from __future__ import annotations

import sys


def main() -> int:
    print(
        "SPEC192_COMPATIBILITY_V1: the Python Repo server was removed; "
        "run the C++ DistributedRepoServer with an explicit Persistent config.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
