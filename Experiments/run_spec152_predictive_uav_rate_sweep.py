#!/usr/bin/env python3
"""Freeze and execute the immutable Spec 152 six-rate MiniNDN campaign."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/run_spec151_predictive_bounded_catchup.py"
ANALYZER = ROOT / "Experiments/analyze_spec152_predictive_uav_rate_sweep.py"
RUNNER = Path(__file__).resolve()
RATES = (10, 20, 30, 40, 50, 60)


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec151_runner_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load runner base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()
ENGINE = BASE.BASE.BASE
ENGINE.ANALYZER = ANALYZER
ENGINE.RUNNER = RUNNER
ENGINE.PROFILES = tuple({
    "cellId": f"fps-{fps:02d}",
    "fps": fps,
    "lossPercent": 0.0,
    "delayMs": 0.0,
    "jitterMs": 0.0,
    "reorderPercent": 0.0,
    "reorderCorrelationPercent": 0.0,
    "reorderGap": 0,
} for fps in RATES)
ENGINE.CAMPAIGN_ENV = {
    **ENGINE.CAMPAIGN_ENV,
    "LD_LIBRARY_PATH": f"{ROOT / 'build'}:{ROOT / '.local-boost171/lib'}",
}
ENGINE.SOURCE_PATHS = tuple(Path(value) for value in (
    "ndn-service-framework/Stream.hpp",
    "ndn-service-framework/Stream.cpp",
    "ndn-service-framework/StreamFacade.hpp",
    "ndn-service-framework/StreamFacade.cpp",
    "NDNSF-UAV-APP/shared/UavVideoPipeline.hpp",
    "NDNSF-UAV-APP/shared/UavVideoPipeline.cpp",
    "NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp",
    "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp",
    "Experiments/NDNSF_UAV_GUI_Minindn.py",
    "Experiments/analyze_stream_latency.py",
    "Experiments/analyze_spec148_predictive_uav.py",
    "Experiments/analyze_spec149_predictive_recovery.py",
    "Experiments/analyze_spec150_predictive_ordered_drain.py",
    "Experiments/analyze_spec151_predictive_bounded_catchup.py",
    "Experiments/analyze_spec152_predictive_uav_rate_sweep.py",
    "Experiments/run_spec148_predictive_uav_acceptance.py",
    "Experiments/run_spec149_predictive_recovery.py",
    "Experiments/run_spec150_predictive_ordered_drain.py",
    "Experiments/run_spec151_predictive_bounded_catchup.py",
    "Experiments/run_spec152_predictive_uav_rate_sweep.py",
    "specs/152-predictive-uav-rate-sweep/spec.md",
    "specs/152-predictive-uav-rate-sweep/plan.md",
    "specs/152-predictive-uav-rate-sweep/tasks.md",
    "specs/152-predictive-uav-rate-sweep/contracts/rate-sweep.md",
    "Experiments/Topology/AI_Lab.conf",
    "NDNSF-UAV-APP/configs/uav_runtime.conf",
    "NDNSF-UAV-APP/configs/drone-A.conf",
    "NDNSF-UAV-APP/configs/ground-station.conf",
    "NDNSF-UAV-APP/configs/uav_demo.policies",
    "NDNSF-UAV-APP/videos/drone.mp4",
))


def command_for(cell_dir: Path, profile: dict[str, Any]) -> list[str]:
    command = ENGINE.command_for_original(cell_dir, profile)
    index = command.index("--video-fps") + 1
    command[index] = str(profile["fps"])
    return command


ENGINE.command_for_original = ENGINE.command_for
ENGINE.command_for = command_for


def replace_schema(value: Any) -> Any:
    if isinstance(value, str):
        return value.replace(
            "spec148-predictive-uav", "spec152-predictive-uav-rate"
        )
    if isinstance(value, list):
        return [replace_schema(item) for item in value]
    if isinstance(value, dict):
        return {key: replace_schema(item) for key, item in value.items()}
    return value


def verify_runtime_linkage() -> None:
    environment = os.environ.copy()
    environment.update(ENGINE.CAMPAIGN_ENV)
    expected = str(ROOT / "build/libndn-service-framework.so.0.1.0")
    for binary in (
        ROOT / "build/examples/UavDroneApp",
        ROOT / "build/examples/UavGroundStationApp",
    ):
        completed = subprocess.run(
            ["ldd", str(binary)], cwd=ROOT, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )
        if completed.returncode != 0 or expected not in completed.stdout:
            raise RuntimeError(
                f"{binary.name} does not resolve build Core:\n{completed.stdout}"
            )


def prepare(output_root: Path) -> None:
    verify_runtime_linkage()
    ENGINE.prepare(output_root)
    path = output_root / "frozen-campaign.json"
    campaign = replace_schema(json.loads(path.read_text(encoding="utf-8")))
    campaign["formalCellCount"] = len(RATES)
    path.write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def write_comparison(output_root: Path, summary: dict[str, Any]) -> None:
    rows: list[dict[str, Any]] = []
    for cell in summary["cells"]:
        metrics = cell.get("metrics", {})
        latency = metrics.get("endToEndAoIMs", {})
        rows.append({
            "cell": cell.get("cellId"),
            "accepted": cell.get("accepted", False),
            "requested_fps": metrics.get("requestedFps"),
            "achieved_fps": metrics.get("achievedFps"),
            "rate_error_percent": (
                100 * metrics["frameRateRelativeError"]
                if metrics.get("frameRateRelativeError") is not None else None
            ),
            "segment_publish_pps": metrics.get("segmentPublishPps"),
            "segment_delivery_pps": metrics.get("segmentDeliveryPps"),
            "delivery_percent": (
                100 * metrics["deliveryRatio"]
                if metrics.get("deliveryRatio") is not None else None
            ),
            "aoi_mean_ms": latency.get("mean"),
            "aoi_p50_ms": latency.get("p50"),
            "aoi_p95_ms": latency.get("p95"),
            "aoi_p99_ms": latency.get("p99"),
            "longest_gap_ms": metrics.get("longestDeliveryGapMs"),
            "future_hit_percent": (
                100 * metrics["futureHitRatio"]
                if metrics.get("futureHitRatio") is not None else None
            ),
            "mapping_interests": metrics.get("mappingInterests"),
            "payload_interests": metrics.get("payloadInterests"),
            "retry": metrics.get("retryAttempts"),
            "timeout": metrics.get("timeouts"),
            "nack": metrics.get("nacks"),
            "recoveries": metrics.get("recoveries"),
            "useless_interest_percent": (
                100 * metrics["uselessInterestRatio"]
                if metrics.get("uselessInterestRatio") is not None else None
            ),
        })
    fields = list(rows[0]) if rows else []
    with (output_root / "rate-comparison.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    lines = [
        "# Spec 152 UAV predictive rate comparison", "",
        f"Status: **{summary['status']}**",
        f"Accepted cells: {summary['acceptedCellCount']}/{summary['formalCellCount']}",
        "",
        "| FPS requested | FPS achieved | Rate error | Delivery | AoI p50 | AoI p95 | AoI p99 | Longest gap | Future hit | Mapping | Retry | Timeout | Nack |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['requested_fps']} | {row['achieved_fps']:.3f} | "
            f"{row['rate_error_percent']:.2f}% | {row['delivery_percent']:.3f}% | "
            f"{row['aoi_p50_ms']:.3f} | {row['aoi_p95_ms']:.3f} | "
            f"{row['aoi_p99_ms']:.3f} | {row['longest_gap_ms']:.3f} | "
            f"{row['future_hit_percent']:.3f}% | {row['mapping_interests']} | "
            f"{row['retry']} | {row['timeout']} | {row['nack']} |"
        )
    (output_root / "rate-comparison.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def execute(output_root: Path) -> int:
    verify_runtime_linkage()
    result = ENGINE.execute(output_root)
    summary_path = output_root / "campaign-summary.json"
    if summary_path.is_file():
        summary = replace_schema(
            json.loads(summary_path.read_text(encoding="utf-8"))
        )
        summary_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        write_comparison(output_root, summary)
    return result


def diagnose(output_root: Path, fps: int) -> int:
    """Run one explicitly non-formal 20-second harness qualification cell."""
    verify_runtime_linkage()
    if output_root.exists():
        raise SystemExit(f"refusing to overwrite diagnostic root: {output_root}")
    output_root.mkdir(parents=True)
    profile = next(value for value in ENGINE.PROFILES if value["fps"] == fps)
    command = command_for(output_root, profile)
    command[command.index("--auto-stop-seconds") + 1] = "20"
    cell = {
        "schemaVersion": "spec152-predictive-uav-rate-diagnostic-v1",
        "cellId": f"diagnostic-fps-{fps:02d}",
        "profile": profile,
        "formal": False,
        "expectedShortWindow": True,
        "command": command,
    }
    manifest = output_root / "manifest.json"
    manifest.write_text(
        json.dumps(cell, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (output_root / "command.txt").write_text(
        " ".join(command) + "\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    environment.update(ENGINE.CAMPAIGN_ENV)
    with (output_root / "campaign-launcher.log").open(
        "w", encoding="utf-8"
    ) as log:
        completed = subprocess.run(
            command, cwd=ROOT, env=environment,
            stdout=log, stderr=subprocess.STDOUT, check=False,
        )
    analyzed = subprocess.run(
        [
            sys.executable, str(ANALYZER),
            "--cell-dir", str(output_root),
            "--manifest", str(manifest),
            "--return-code", str(completed.returncode),
        ],
        cwd=ROOT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    )
    (output_root / "analyzer.log").write_text(
        analyzed.stdout, encoding="utf-8"
    )
    summary_path = output_root / "cell-summary.json"
    if not summary_path.is_file():
        return 1
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    # A 20-second qualification cell validates wiring, pacing, and accounting.
    # Steady-state future-hit and tail distributions are formal-only: the
    # fixed pending-interest frontier and startup outliers dominate a short
    # denominator even when all useful transport checks pass.
    ignored = {
        "measurementWindowAtLeast60Seconds",
        "futureHitAtLeast95Percent",
        "endToEndP99AtMost1000Ms",
        "longestGapAtMost1000Ms",
    }
    failed = {
        key: value for key, value in summary["checks"].items()
        if not value and key not in ignored
    }
    qualified = (
        completed.returncode == 0
        and not failed
        and summary["checks"].get("configuredFrameRateWithinFivePercent", False)
    )
    result = {
        "schemaVersion": "spec152-diagnostic-result-v1",
        "qualified": qualified,
        "ignoredFormalOnlyChecks": sorted(ignored),
        "failedChecks": failed,
        "requestedFps": summary["metrics"]["requestedFps"],
        "achievedFps": summary["metrics"]["achievedFps"],
        "rateError": summary["metrics"]["frameRateRelativeError"],
        "deliveryRatio": summary["metrics"]["deliveryRatio"],
    }
    (output_root / "diagnostic-summary.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if qualified else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--prepare-only", action="store_true")
    action.add_argument("--execute", action="store_true")
    action.add_argument("--diagnostic-rate", type=int, choices=(10, 60))
    args = parser.parse_args()
    output_root = args.output_root.resolve()
    if args.prepare_only:
        prepare(output_root)
        return 0
    if args.diagnostic_rate is not None:
        return diagnose(output_root, args.diagnostic_rate)
    return execute(output_root)


if __name__ == "__main__":
    raise SystemExit(main())
