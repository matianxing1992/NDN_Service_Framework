#!/usr/bin/env python3
"""Analyze one Spec 152 predictive UAV rate-sweep cell."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
from pathlib import Path
import re
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "Experiments/analyze_spec151_predictive_bounded_catchup.py"
FRAME_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*event=encoded-output-ready "
    r".*requestId=/NDNSF/UAV/VIDEO/FRAME/",
    re.MULTILINE,
)
PUSH_TIME_RE = re.compile(
    r"^(\d+(?:\.\d+)?)\s+.*STREAM_PUSH ", re.MULTILINE
)


def load_base() -> Any:
    spec = importlib.util.spec_from_file_location("spec151_analyzer_base", BASE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load analyzer base: {BASE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()


def count_in_window(pattern: re.Pattern[str], text: str,
                    start: float | None, end: float | None) -> int:
    if start is None or end is None or end <= start:
        return 0
    return sum(start <= float(value) <= end for value in pattern.findall(text))


def analyze(cell_dir: Path, manifest: dict[str, Any],
            return_code: int) -> dict[str, Any]:
    summary = BASE.analyze(cell_dir, manifest, return_code)
    drone = (cell_dir / "drone.log").read_text(
        encoding="utf-8", errors="replace"
    )
    measurement = summary["measurement"]
    start = measurement["startTimestamp"]
    end = measurement["endTimestamp"]
    seconds = float(measurement["seconds"])
    frame_count = count_in_window(FRAME_RE, drone, start, end)
    measured_pushes = count_in_window(PUSH_TIME_RE, drone, start, end)
    requested = float(manifest["profile"]["fps"])
    achieved = frame_count / seconds if seconds > 0 else 0.0
    error = abs(achieved - requested) / requested if requested > 0 else 1.0
    metrics = summary["metrics"]
    metrics.update({
        "requestedFps": requested,
        "measuredFrameCount": frame_count,
        "achievedFps": achieved,
        "frameRateRelativeError": error,
        "measuredSegmentPushes": measured_pushes,
        "segmentPublishPps": measured_pushes / seconds if seconds > 0 else 0.0,
        "segmentDeliveryPps": (
            metrics["measuredAdmissions"] / seconds if seconds > 0 else 0.0
        ),
    })
    checks = summary["checks"]
    checks.update({
        "configuredFrameRateWithinFivePercent": (
            frame_count > 0 and error <= 0.05
        ),
        "deliveryAtLeast98Percent": metrics["deliveryRatio"] >= 0.98,
        "futureHitAtLeast95Percent": metrics["futureHitRatio"] >= 0.95,
        "endToEndP99AtMost1000Ms": (
            metrics["endToEndAoIMs"]["p99"] is not None
            and float(metrics["endToEndAoIMs"]["p99"]) <= 1000.0
        ),
        "longestGapAtMost1000Ms": metrics["longestDeliveryGapMs"] <= 1000.0,
    })
    summary["schemaVersion"] = "spec152-predictive-uav-rate-cell-v1"
    summary["accepted"] = all(checks.values())
    return summary


def write_outputs(cell_dir: Path, summary: dict[str, Any]) -> None:
    BASE.write_outputs(cell_dir, summary)
    metrics = summary["metrics"]
    rate = {
        "requested_fps": metrics["requestedFps"],
        "achieved_fps": metrics["achievedFps"],
        "frame_rate_relative_error": metrics["frameRateRelativeError"],
        "measured_frames": metrics["measuredFrameCount"],
        "segment_publish_pps": metrics["segmentPublishPps"],
        "segment_delivery_pps": metrics["segmentDeliveryPps"],
    }
    (cell_dir / "rate-summary.json").write_text(
        json.dumps(rate, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    with (cell_dir / "rate-summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rate))
        writer.writeheader()
        writer.writerow(rate)
    with (cell_dir / "analysis.md").open("a", encoding="utf-8") as stream:
        stream.write("\n## Frame rate and segment rate\n\n")
        stream.write("| Metric | Value |\n|---|---:|\n")
        for key, value in rate.items():
            stream.write(f"| {key} | {value} |\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cell-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--return-code", required=True, type=int)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    summary = analyze(args.cell_dir, manifest, args.return_code)
    write_outputs(args.cell_dir, summary)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
