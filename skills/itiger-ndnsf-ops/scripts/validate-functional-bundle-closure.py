#!/usr/bin/env python3
"""Check the identity/configuration closure of a Spec175 functional bundle.

This validator is intentionally independent of Apptainer, SSH, Slurm, and
PyYAML.  It catches the failure mode where a Controller wrapper extracts
bootstrap tokens for identities that the staged policy never causes the
Controller to create.  Such a mismatch can terminate the wrapper under
``set -e`` and remove the Controller Face/routes before Providers bootstrap.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any


IDENTITY = re.compile(r"/[A-Za-z0-9._~-]+(?:/[A-Za-z0-9._~-]+)*")
POLICY_IDENTITY = re.compile(
    r"(?m)^\s*-?\s*(?:identity|user_identity):\s*([^\s#]+)\s*$")
TOKEN_CALL = re.compile(
    r"(?m)^\s*bootstrap_token_for_identity\s+(?:\"([^\"]+)\"|([^\s]+))")
HOME_ASSIGNMENT = re.compile(
    r"(?:^|[;\s])(?:export\s+)?HOME=([^;\s]+)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _clean_identity(value: str) -> str:
    value = value.strip().strip("'\"")
    value = value.split("${", 1)[0]
    value = value.rstrip("/;)")
    return value if IDENTITY.fullmatch(value) else ""


def required_identities(wrapper: str) -> list[str]:
    values: list[str] = []
    for match in TOKEN_CALL.finditer(wrapper):
        raw = match.group(1) or match.group(2) or ""
        if "${suffix}" in raw:
            base = raw.split("${suffix}", 1)[0].rstrip("/")
            values.extend([base, f"{base}/1", f"{base}/2"])
        else:
            value = _clean_identity(raw)
            if value:
                values.append(value)
    return sorted(set(values))


def policy_identities(policy: str) -> list[str]:
    values = []
    for match in POLICY_IDENTITY.finditer(policy):
        value = _clean_identity(match.group(1))
        if value:
            values.append(value)
    return sorted(set(values))


def process_home_paths(bundle: Path) -> tuple[dict[str, str], list[str]]:
    """Return process->HOME bindings and shared/missing HOME errors.

    The exact-SIF runner launches the Controller and Providers concurrently.
    Sharing the default ``$HOME/.ndn`` PIB across those processes is a
    deterministic SQLite lock race, so this check is intentionally performed
    on the mutable functional bundle before any allocation.
    """
    errors: list[str] = []
    files: list[tuple[str, Path]] = [("controller", bundle / "controller-wrapper.sh"),
                                     ("user", bundle / "user.args")]
    provider_dir = bundle / "providers"
    if provider_dir.is_dir():
        files.extend(
            (f"provider:{path.stem}", path)
            for path in sorted(provider_dir.glob("*.args")))
    homes: dict[str, str] = {}
    for label, path in files:
        if not path.is_file():
            errors.append(f"{label}: missing launch file for HOME/PIB check")
            continue
        text = path.read_text(encoding="utf-8")
        matches = [value.strip().strip("'\"")
                   for value in HOME_ASSIGNMENT.findall(text)]
        if not matches:
            errors.append(f"{label}: no explicit HOME assignment")
            continue
        if len(set(matches)) != 1:
            errors.append(f"{label}: multiple HOME assignments: {matches}")
            continue
        homes[label] = matches[0]
    by_home: dict[str, list[str]] = {}
    for label, home in homes.items():
        by_home.setdefault(home, []).append(label)
    for home, labels in sorted(by_home.items()):
        if len(labels) > 1:
            errors.append(
                "shared HOME/PIB path " + home + " used by " + ", ".join(labels))
    return homes, errors


def validate(bundle: Path) -> dict[str, Any]:
    errors: list[str] = []
    wrapper_path = bundle / "controller-wrapper.sh"
    policy_path = bundle / "policy.yaml"
    if not wrapper_path.is_file():
        errors.append("missing controller-wrapper.sh")
    if not policy_path.is_file():
        errors.append("missing policy.yaml")
    if errors:
        return {"schemaVersion": "ndnsf-functional-bundle-closure-v1",
                "status": "FAIL", "bundle": str(bundle), "errors": errors}

    wrapper = wrapper_path.read_text(encoding="utf-8")
    policy = policy_path.read_text(encoding="utf-8")
    required = required_identities(wrapper)
    declared = policy_identities(policy)
    missing = sorted(set(required) - set(declared))
    if not required:
        errors.append("wrapper has no statically discoverable bootstrap identities")
    if missing:
        errors.append(
            "bootstrap identities required by wrapper are absent from policy: "
            + ", ".join(missing))
    homes, home_errors = process_home_paths(bundle)
    errors.extend(home_errors)

    return {
        "schemaVersion": "ndnsf-functional-bundle-closure-v1",
        "status": "PASS" if not errors else "FAIL",
        "bundle": str(bundle),
        "wrapperSha256": sha256(wrapper_path),
        "policySha256": sha256(policy_path),
        "requiredBootstrapIdentities": required,
        "declaredPolicyIdentities": declared,
        "missingBootstrapIdentities": missing,
        "processHomes": homes,
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = validate(args.bundle.expanduser().resolve())
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
