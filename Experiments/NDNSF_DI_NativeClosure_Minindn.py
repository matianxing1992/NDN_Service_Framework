#!/usr/bin/env python3
"""MiniNDN owner for the Spec182 native-closure runner.

This module deliberately does not implement a second collector or produce a
business result.  A future MiniNDN campaign supplies real node/netns metadata
and invokes the same standalone runner used by the local qualification gate.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tests/standalone/run-spec182-native-closure.py"
MANIFEST_SCHEMA = "spec182-case-manifest-v1"
QUALIFICATION_SCHEMA = "spec182-native-qualification-v1"
COUNTEREXAMPLES = tuple(f"I{index:02d}" for index in range(1, 9))
PROOF_CASES = tuple(f"PO-{index:03d}" for index in range(1, 15))


def _runner_module():
    spec = importlib.util.spec_from_file_location("spec182_native_closure", RUNNER)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Spec182 native closure runner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_registration(manifest_path: Path) -> dict[str, Any]:
    """Validate the frozen campaign registration without starting MiniNDN.

    Registration is deliberately separate from execution: this function only
    checks that the frozen manifest names the one shared runner, all I01--I08
    detector counterexamples, and the PO-001--PO-014 acceptance owners.
    """
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    if document.get("schema") != MANIFEST_SCHEMA:
        raise ValueError("case-manifest schema mismatch")
    registration = document.get("qualification")
    if not isinstance(registration, dict):
        raise ValueError("qualification registration is missing")
    if registration.get("schema") != QUALIFICATION_SCHEMA:
        raise ValueError("qualification schema mismatch")
    runner = registration.get("runner")
    if runner != "tests/standalone/run-spec182-native-closure.py":
        raise ValueError("qualification runner is not canonical")
    cases = registration.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("qualification cases are missing")
    ids: list[str] = []
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("id"), str):
            raise ValueError("qualification case entry is invalid")
        if case["id"] in ids:
            raise ValueError("qualification case id is duplicated")
        if case.get("executeOwner") != "T016":
            raise ValueError(f"qualification case owner is not T016: {case['id']}")
        if case.get("expectedStatus") not in {"PASS", "FAIL", "UNQUALIFIED"}:
            raise ValueError(f"qualification case status is invalid: {case['id']}")
        ids.append(case["id"])
    required = set(COUNTEREXAMPLES) | set(PROOF_CASES)
    missing = sorted(required - set(ids))
    if missing:
        raise ValueError("qualification cases are missing: " + ",".join(missing))
    limits = registration.get("limits")
    if not isinstance(limits, dict) or int(limits.get("runSeconds", 0)) <= 0 \
            or int(limits.get("cleanupSeconds", 0)) <= 0:
        raise ValueError("qualification limits are invalid")
    return registration


def run_campaign(manifest_path: Path, output: Path) -> int:
    """Run one manifest-selected case after an external MiniNDN owner setup.

    The manifest must select exactly one case through ``campaignCase``.  Real
    node contexts are intentionally passed by the eventual MiniNDN harness;
    this entry point returns UNQUALIFIED until that owner supplies them.
    """
    output = output.resolve()
    output_preexisting = output.exists()
    try:
        if output_preexisting:
            raise ValueError("output run directory must be new")
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        case_id = document.get("campaignCase", "")
        if not isinstance(case_id, str) or not case_id:
            raise ValueError("campaignCase is required")
        registration = load_registration(manifest_path)
        registered_ids = {case["id"] for case in registration["cases"]}
        if case_id not in registered_ids:
            raise ValueError("campaignCase is not registered")
        output.mkdir(parents=True, exist_ok=False)
        (output / "result.json").write_text(
            json.dumps({
                "status": "UNQUALIFIED",
                "reason": "MININDN_NODE_CONTEXT_NOT_PROVIDED",
                "campaignCase": case_id,
                "runner": registration["runner"],
                "registeredCases": sorted(registered_ids),
                "limits": registration["limits"],
            }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 2
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        if output_preexisting:
            return 2
        if not output.exists():
            try:
                output.mkdir(parents=True, exist_ok=False)
            except OSError:
                return 2
        if output.is_dir():
            (output / "result.json").write_text(
                json.dumps({"status": "UNQUALIFIED", "reason": str(exc)},
                           sort_keys=True) + "\n", encoding="utf-8")
        return 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    return run_campaign(args.manifest, args.output)


if __name__ == "__main__":
    raise SystemExit(main())
