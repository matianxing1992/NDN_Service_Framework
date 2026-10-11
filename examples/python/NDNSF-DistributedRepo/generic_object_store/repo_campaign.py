#!/usr/bin/env python3
"""Retired Python Repo campaign runner."""

from __future__ import annotations

import sys


def main() -> int:
    print(
        "SPEC192_COMPATIBILITY_V1: this Python Repo campaign used a retired "
        "runtime; use the current C++ Repo integration and MiniNDN gates.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
