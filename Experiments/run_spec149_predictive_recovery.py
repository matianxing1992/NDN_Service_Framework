#!/usr/bin/env python3
"""Freeze and execute the immutable two-cell Spec 149 campaign."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/run_spec148_predictive_uav_acceptance.py"
ANALYZER = ROOT / "Experiments/analyze_spec149_predictive_recovery.py"
RUNNER = Path(__file__).resolve()


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec148_runner_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load runner base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()
BASE.ANALYZER = ANALYZER
BASE.RUNNER = RUNNER
BASE.SOURCE_PATHS = tuple(Path(value) for value in (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "ndn-service-framework/StreamFacade.hpp",
    "ndn-service-framework/StreamFacade.cpp",
    "ndn-service-framework/ServiceProvider.hpp",
    "ndn-service-framework/ServiceProvider.cpp",
    "ndn-service-framework/ServiceUser.hpp",
    "ndn-service-framework/ServiceUser.cpp",
    "pythonWrapper/ndnsf/streaming.py",
    "pythonWrapper/ndnsf/service.py",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "NDNSF-UAV-APP/shared/UavProtocol.hpp",
    "NDNSF-UAV-APP/shared/UavProtocol.cpp",
    "NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp",
    "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp",
    "Experiments/NDNSF_UAV_GUI_Minindn.py",
    "Experiments/analyze_stream_latency.py",
    "Experiments/analyze_spec148_predictive_uav.py",
    "Experiments/run_spec148_predictive_uav_acceptance.py",
    "Experiments/analyze_spec149_predictive_recovery.py",
    "Experiments/run_spec149_predictive_recovery.py",
    "specs/149-predictive-recovery-coalescing/spec.md",
    "specs/149-predictive-recovery-coalescing/plan.md",
    "specs/149-predictive-recovery-coalescing/tasks.md",
    "specs/149-predictive-recovery-coalescing/contracts/recovery-control.md",
    "Experiments/Topology/AI_Lab.conf",
    "NDNSF-UAV-APP/configs/uav_runtime.conf",
    "NDNSF-UAV-APP/configs/drone-A.conf",
    "NDNSF-UAV-APP/configs/ground-station.conf",
    "NDNSF-UAV-APP/configs/uav_demo.policies",
    "NDNSF-UAV-APP/videos/drone.mp4",
))


def replace_schema(value: Any) -> Any:
    if isinstance(value, str):
        return value.replace("spec148-predictive-uav", "spec149-predictive-recovery")
    if isinstance(value, list):
        return [replace_schema(item) for item in value]
    if isinstance(value, dict):
        return {key: replace_schema(item) for key, item in value.items()}
    return value


def prepare(output_root: Path) -> None:
    BASE.prepare(output_root)
    campaign_path = output_root / "frozen-campaign.json"
    campaign = replace_schema(json.loads(campaign_path.read_text(encoding="utf-8")))
    campaign_path.write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def execute(output_root: Path) -> int:
    result = BASE.execute(output_root)
    summary_path = output_root / "campaign-summary.json"
    if summary_path.is_file():
        summary = replace_schema(
            json.loads(summary_path.read_text(encoding="utf-8"))
        )
        summary_path.write_text(
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
