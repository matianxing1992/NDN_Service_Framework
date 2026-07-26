#!/usr/bin/env python3
"""Freeze and execute the immutable Spec 154 six-rate confirmation."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/run_spec153_uav_decode_stop_repair.py"
RUNNER = Path(__file__).resolve()


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec153_runner_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load runner base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()
BASE.BASE.ENGINE.RUNNER = RUNNER
BASE.BASE.ENGINE.SOURCE_PATHS = BASE.BASE.ENGINE.SOURCE_PATHS + tuple(
    Path(value) for value in (
        "Experiments/run_spec154_uav_stop_process_map.py",
        "specs/154-uav-stop-process-map-confirmation/spec.md",
        "specs/154-uav-stop-process-map-confirmation/plan.md",
        "specs/154-uav-stop-process-map-confirmation/tasks.md",
        "specs/154-uav-stop-process-map-confirmation/contracts/formal-matrix.md",
    )
)


def replace_schema(value: Any) -> Any:
    if isinstance(value, str):
        return value.replace(
            "spec153-uav-decode-stop-repair",
            "spec154-uav-stop-process-map",
        )
    if isinstance(value, list):
        return [replace_schema(item) for item in value]
    if isinstance(value, dict):
        return {key: replace_schema(item) for key, item in value.items()}
    return value


def prepare(output_root: Path) -> None:
    BASE.prepare(output_root)
    path = output_root / "frozen-campaign.json"
    campaign = replace_schema(json.loads(path.read_text(encoding="utf-8")))
    path.write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def execute(output_root: Path) -> int:
    result = BASE.execute(output_root)
    path = output_root / "campaign-summary.json"
    if path.is_file():
        summary = replace_schema(json.loads(path.read_text(encoding="utf-8")))
        path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--prepare-only", action="store_true")
    action.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    output_root = args.output_root.resolve()
    if args.prepare_only:
        prepare(output_root)
        return 0
    return execute(output_root)


if __name__ == "__main__":
    raise SystemExit(main())
