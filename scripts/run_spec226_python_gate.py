#!/usr/bin/env python3
"""Run the Spec226 installed-binding and actor-keychain unit gates."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
TESTS = (
    ROOT / "tests/python/test_spec226_actor_keychains.py",
    ROOT / "tests/python/test_spec226_compatibility_bindings.py",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", default=sys.executable,
                        help="Python interpreter used for the unit test")
    args = parser.parse_args()
    missing = [path for path in TESTS if not path.is_file()]
    if missing:
        print("Spec226 Python gate is missing tests: " + ", ".join(map(str, missing)),
              file=sys.stderr)
        return 2
    for test in TESTS:
        result = subprocess.run([args.python, str(test)], cwd=ROOT, check=False)
        if result.returncode != 0:
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
