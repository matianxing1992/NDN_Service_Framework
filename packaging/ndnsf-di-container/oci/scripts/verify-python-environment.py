#!/usr/bin/env python3
"""Verify the installed Python closure with explicit CUDA-base exceptions."""

from __future__ import annotations

import argparse
import importlib.metadata as metadata
import json
from pathlib import Path

from packaging.requirements import Requirement
from packaging.specifiers import SpecifierSet
from packaging.utils import canonicalize_name


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True)
    args = parser.parse_args()
    lock = json.loads(Path(args.lock).read_text())
    wanted = {
        canonicalize_name(name): version
        for name, version in lock["pythonPackages"].items()
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
    print(
        f"PYTHON_ENVIRONMENT_PASS:locked={len(wanted)}:systemProvided={len(system)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
