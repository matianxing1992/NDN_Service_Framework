#!/usr/bin/env python3
"""Generate deterministic Spec191 UAV motion and calibration input."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "NDNSF-UAV-APP/tracking"))
from motion_schema import CalibrationProfile, TelemetrySample


CAMERAS = ("UAV1", "UAV2", "UAV3")
ORIGIN_LAT_E7 = 350000000
ORIGIN_LON_E7 = -900000000
MOTION_PROFILES = (
    "nominal",
    "missing-calibration",
    "stale-telemetry",
    "reversed-homography",
    "pts-disorder",
    "camera-mismatch",
    "disabled-ego-motion",
)


def _lat_e7(metres: float) -> int:
    return ORIGIN_LAT_E7 + round(metres / 111_320.0 * 1e7)


def _lon_e7(metres: float) -> int:
    return ORIGIN_LON_E7 + round(metres / (111_320.0 * 0.819) * 1e7)


def build_window(window: int, *, period_us: int = 1_000_000,
                 profile: str = "nominal") -> dict[str, Any]:
    if window < 0:
        raise ValueError("window must be non-negative")
    if profile not in MOTION_PROFILES:
        raise ValueError(f"unsupported motion profile: {profile}")
    pts = window * period_us
    telemetry: list[dict[str, Any]] = []
    calibrations: list[dict[str, Any]] = []
    for index, camera in enumerate(CAMERAS):
        speed = 4_000 + index * 1_000 + (window % 2) * 250
        x = window * (speed / 1000.0)
        y = float(index * 8)
        sample = TelemetrySample(
            drone_id=camera, camera_id=camera, source_epoch=1, sequence=window + 1,
            source_timestamp_us=pts, latitude_e7=_lat_e7(y), longitude_e7=_lon_e7(x),
            altitude_mm=80_000, heading_cdeg=0, ground_speed_mmps=speed,
        )
        sample.validate()
        telemetry.append(sample.to_dict())
        calibration = CalibrationProfile(
            camera_id=camera, image_width=1920, image_height=1080,
            # Pixel -> ground millimetres, with a fixed downward camera frame.
            homography=((10.0, 0.0, -9600.0), (0.0, 10.0, -5400.0),
                        (0.0, 0.0, 1.0)),
        )
        calibrations.append(calibration.to_dict())
    # This is intentionally independent from the UAV speed and never copied
    # into TrackRecord or the response as an estimate.
    oracle_speed = 6_000
    value = {
        "schema": "spec191-motion-window-v1",
        "windowId": f"window-{window}",
        "sourceTimestampUs": pts,
        "telemetry": telemetry,
        "calibrations": calibrations,
        "vehicleTrajectory": {"vehicleId": "vehicle-01", "worldXmm": window * oracle_speed,
                               "worldYmm": 1200, "worldSpeedMmps": oracle_speed,
                               "visibility": "simulated-input"},
        "oracle": {"vehicleId": "vehicle-01", "worldSpeedMmps": oracle_speed,
                    "provenance": "simulated-input"},
        "inputProvenance": "simulated-input",
        "motionProfile": profile,
    }
    if profile == "missing-calibration":
        value["calibrations"] = []
    elif profile == "stale-telemetry":
        for sample in value["telemetry"]:
            sample["freshness"] = "stale"
    elif profile == "reversed-homography":
        for calibration in value["calibrations"]:
            calibration["homography"][0][0] = -10.0
    elif profile == "pts-disorder" and window > 0:
        disorder = max(0, pts - period_us // 2)
        for sample in value["telemetry"]:
            sample["sourceTimestampUs"] = disorder
        value["sourceTimestampUs"] = disorder
    elif profile == "camera-mismatch":
        value["telemetry"][0]["cameraId"] = "UAVX"
    elif profile == "disabled-ego-motion":
        value["motionPolicy"] = {"egoMotionCompensation": "disabled"}
    return value


def write_fixture(root: Path, window_count: int, *, profile: str = "nominal") -> dict[str, Any]:
    if window_count < 1 or window_count > 60:
        raise ValueError("window_count must be in 1..60")
    if profile not in MOTION_PROFILES:
        raise ValueError(f"unsupported motion profile: {profile}")
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    windows: dict[str, str] = {}
    digests: dict[str, str] = {}
    for index in range(window_count):
        payload = json.dumps(build_window(index, profile=profile), sort_keys=True,
                             separators=(",", ":"))
        path = root / f"window-{index}.json"
        path.write_text(payload + "\n", encoding="utf-8")
        windows[f"window-{index}"] = str(path)
        digests[f"window-{index}"] = "sha256:" + hashlib.sha256(payload.encode()).hexdigest()
    manifest = {"schema": "spec191-motion-fixture-v1", "provenance": "simulated-input",
                "profile": profile, "windowCount": window_count,
                "windows": windows, "digests": digests}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                          encoding="utf-8")
    return manifest


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--window-count", type=int, default=3)
    parser.add_argument("--profile", choices=MOTION_PROFILES, default="nominal")
    args = parser.parse_args()
    print(json.dumps(write_fixture(args.output, args.window_count, profile=args.profile),
                     indent=2, sort_keys=True))
