#!/usr/bin/env python3
"""MiniNDN owner for the Spec182 native-closure runner.

This module deliberately does not implement a second collector or produce a
business result.  A future MiniNDN campaign supplies real node/netns metadata
and invokes the same standalone runner used by the local qualification gate.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tests/standalone/run-spec182-native-closure.py"


def _runner_module():
    spec = importlib.util.spec_from_file_location("spec182_native_closure", RUNNER)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Spec182 native closure runner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_campaign(manifest_path: Path, output: Path) -> int:
    """Run one manifest-selected case after an external MiniNDN owner setup.

    The manifest must select exactly one case through ``campaignCase``.  Real
    node contexts are intentionally passed by the eventual MiniNDN harness;
    this entry point returns UNQUALIFIED until that owner supplies them.
    """
    try:
        document = _runner_module().json.loads(manifest_path.read_text(encoding="utf-8"))
        case_id = document.get("campaignCase", "")
        if not isinstance(case_id, str) or not case_id:
            raise ValueError("campaignCase is required")
        result = output / "result.json"
        output.mkdir(parents=True, exist_ok=False)
        result.write_text(
            '{"status":"UNQUALIFIED","reason":"MININDN_NODE_CONTEXT_NOT_PROVIDED"}\n',
            encoding="utf-8")
        return 2
    except (OSError, ValueError) as exc:
        output.mkdir(parents=True, exist_ok=True)
        (output / "result.json").write_text(
            '{"status":"UNQUALIFIED","reason":"%s"}\n' % str(exc).replace('"', "'"),
            encoding="utf-8")
        return 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    return run_campaign(args.manifest, args.output)


if __name__ == "__main__":
    raise SystemExit(main())
