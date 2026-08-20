#!/usr/bin/env python3
"""Verify the installed Python closure with explicit CUDA-base exceptions."""

from __future__ import annotations

import argparse
import importlib.metadata as metadata
import json
from pathlib import Path
import re
import subprocess
import sys

from packaging.requirements import Requirement
from packaging.specifiers import SpecifierSet
from packaging.utils import canonicalize_name


def _requirement_name(value: str) -> str:
    """Return a normalized distribution name from pip's requirement text."""
    token = value.strip().split()[0]
    return canonicalize_name(re.split(r"[<>=!~]", token, maxsplit=1)[0])


def check_pip_dependencies(system: dict[str, str]) -> int:
    """Run pip check while honoring only declared system CUDA packages.

    CUDA images provide the native runtime libraries, not Python ``*.dist-info``
    records.  Consequently pip reports those packages as missing even though
    the image contract intentionally supplies them outside the venv.  Every
    other pip-check failure remains fatal.
    """
    result = subprocess.run(
        [sys.executable, "-m", "pip", "check"],
        check=False,
        capture_output=True,
        text=True,
    )
    lines = [
        line.strip()
        for line in (result.stdout + "\n" + result.stderr).splitlines()
        if line.strip()
    ]
    allowed: list[str] = []
    unexpected: list[str] = []
    for line in lines:
        match = re.search(
            r"requires\s+([^,]+),\s+which is not installed\.$", line
        )
        if match and _requirement_name(match.group(1)) in system:
            allowed.append(line)
        else:
            unexpected.append(line)
    if result.returncode != 0 and not lines:
        unexpected.append(f"PIP_CHECK_EXIT:{result.returncode}")
    if unexpected:
        raise SystemExit("\n".join(sorted(set(unexpected))))
    print(f"PIP_CHECK_PASS:systemProvidedExceptions={len(allowed)}")
    return len(allowed)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True)
    parser.add_argument(
        "--pip-check",
        action="store_true",
        help="run pip check with only lock-declared system CUDA exceptions",
    )
    parser.add_argument(
        "--package-set",
        default="pythonPackages",
        choices=("pythonPackages", "deploymentPythonPackages"),
        help="lock key describing the environment being verified",
    )
    args = parser.parse_args()
    lock = json.loads(Path(args.lock).read_text())
    package_rows = lock.get(args.package_set)
    if not isinstance(package_rows, dict):
        raise SystemExit(f"PYTHON_PACKAGE_SET_MISSING:{args.package_set}")
    if args.package_set == "deploymentPythonPackages":
        forbidden = {"torch", "transformers"}
        if forbidden.intersection(canonicalize_name(name) for name in package_rows):
            raise SystemExit("DEPLOYMENT_FORBIDDEN_PYTHON_PACKAGE")
    wanted = {
        canonicalize_name(name): version
        for name, version in package_rows.items()
    }
    system = {
        canonicalize_name(name): version
        for name, version in lock["pythonSystemProvidedPackages"].items()
    }
    installed = {
        canonicalize_name(dist.metadata["Name"]): dist.version
        for dist in metadata.distributions()
        if dist.metadata.get("Name")
    }
    errors = []
    for name, version in sorted(wanted.items()):
        measured = installed.get(name)
        if measured is None or measured not in SpecifierSet(f"=={version}"):
            errors.append(f"PYTHON_LOCK_VERSION_MISMATCH:{name}:{measured}:{version}")
    for name in sorted(wanted):
        try:
            requires = metadata.requires(name) or []
        except metadata.PackageNotFoundError:
            continue
        for value in requires:
            requirement = Requirement(value)
            if requirement.marker and not requirement.marker.evaluate({"extra": ""}):
                continue
            required_name = canonicalize_name(requirement.name)
            if required_name in system:
                continue
            measured = installed.get(required_name)
            if measured is None:
                errors.append(f"PYTHON_DEPENDENCY_MISSING:{name}:{required_name}")
            elif requirement.specifier and measured not in requirement.specifier:
                errors.append(
                    f"PYTHON_DEPENDENCY_VERSION_MISMATCH:{name}:{required_name}:{measured}:{requirement.specifier}"
                )
    if errors:
        raise SystemExit("\n".join(sorted(set(errors))))
    if args.pip_check:
        check_pip_dependencies(system)
    print(
        f"PYTHON_ENVIRONMENT_PASS:locked={len(wanted)}:systemProvided={len(system)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
