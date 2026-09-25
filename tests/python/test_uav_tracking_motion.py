"""Unit coverage for Spec191 injected motion and speed estimation."""

from __future__ import annotations

import json
from pathlib import Path
import sys

import cv2
import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
sys.path.insert(0, str(ROOT / "Experiments/UAV"))

from motion_estimator import estimate_vehicle_speed  # noqa: E402
from motion_fixture import MOTION_PROFILES, build_window, write_fixture  # noqa: E402
from motion_schema import CalibrationProfile, canonical_speed_mmps, load_motion_window  # noqa: E402
from tracking_worker import TrackingEngine  # noqa: E402
from tracking_report import summarize_motion_result  # noqa: E402
from tracking_display import DisplayFrame  # noqa: E402


def test_speed_alias_and_fixture_are_canonical() -> None:
    assert canonical_speed_mmps(groundspeed_mps=4.25) == 4250
    window = build_window(1)
    telemetry, calibrations, oracle = load_motion_window(window)
    assert [sample.camera_id for sample in telemetry] == ["UAV1", "UAV2", "UAV3"]
    assert all(sample.ground_speed_mmps >= 4000 for sample in telemetry)
    assert set(calibrations) == {"UAV1", "UAV2", "UAV3"}
    assert oracle["provenance"] == "simulated-input"
    assert "worldSpeedMmps" not in telemetry[0].to_dict()


def test_nominal_estimate_uses_homography_and_ego_motion() -> None:
    first = load_motion_window(build_window(0))[0][0]
    second = load_motion_window(build_window(1))[0][0]
    profile = load_motion_window(build_window(0))[1]["UAV1"]
    estimate = estimate_vehicle_speed(
        {"globalId": 1, "box": [950, 500, 970, 600], "ptsUs": 0},
        {"globalId": 1, "box": [950, 500, 970, 1000], "ptsUs": 1_000_000},
        first, second, profile)
    assert estimate.status == "ok"
    assert estimate.provenance == "homography-estimated"
    assert estimate.ego_motion_compensation == "applied"
    assert estimate.vehicle_speed_mmps == 4000
    assert estimate.distance_mm == pytest.approx(4000.0)
    disabled = estimate_vehicle_speed(
        {"globalId": 1, "box": [950, 500, 970, 600], "ptsUs": 0},
        {"globalId": 1, "box": [950, 500, 970, 1000], "ptsUs": 1_000_000},
        first, second, profile, require_ego_compensation=False)
    assert disabled.status == "unknown"
    assert disabled.vehicle_speed_mmps is None


def test_missing_calibration_fails_closed() -> None:
    value = {**build_window(0), "calibrations": []}
    with pytest.raises(ValueError):
        load_motion_window(value)


def test_stale_telemetry_is_preserved_for_explicit_unknown() -> None:
    value = build_window(1)
    value["telemetry"][0]["freshness"] = "stale"
    telemetry, _, _ = load_motion_window(value)
    assert telemetry[0].freshness == "stale"


@pytest.mark.parametrize("profile", MOTION_PROFILES[1:])
def test_negative_motion_profiles_are_explicit(profile: str, tmp_path: Path) -> None:
    manifest = write_fixture(tmp_path / profile, 2, profile=profile)
    assert manifest["profile"] == profile
    first = json.loads(Path(manifest["windows"]["window-0"]).read_text())
    if profile == "missing-calibration":
        assert first["calibrations"] == []
    elif profile == "stale-telemetry":
        assert {item["freshness"] for item in first["telemetry"]} == {"stale"}
    elif profile == "reversed-homography":
        assert first["calibrations"][0]["homography"][0][0] < 0
    elif profile == "camera-mismatch":
        assert first["telemetry"][0]["cameraId"] == "UAVX"
    elif profile == "disabled-ego-motion":
        assert first["motionPolicy"]["egoMotionCompensation"] == "disabled"
    else:
        second = json.loads(Path(manifest["windows"]["window-1"]).read_text())
        assert second["sourceTimestampUs"] == 500_000


def test_disabled_ego_motion_is_unknown() -> None:
    first = load_motion_window(build_window(0))[0][0]
    second_window = build_window(1, profile="disabled-ego-motion")
    second = load_motion_window(second_window)[0][0]
    profile = load_motion_window(build_window(0))[1]["UAV1"]
    estimate = estimate_vehicle_speed(
        {"globalId": 1, "box": [950, 500, 970, 600], "ptsUs": 0},
        {"globalId": 1, "box": [950, 500, 970, 1000], "ptsUs": 1_000_000},
        first, second, profile,
        require_ego_compensation=False)
    assert estimate.status == "unknown"
    assert estimate.vehicle_speed_mmps is None


def test_worker_preserves_motion_metadata_without_copying_oracle(tmp_path: Path) -> None:
    image = np.zeros((120, 160, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    frames = [(dict(cameraId=camera, sequence=1, ptsUs=0), encoded.tobytes())
              for camera in ("UAV1", "UAV2", "UAV3")]
    engine = TrackingEngine(functional=True)
    result = engine.process_window({"runId": "r", "missionId": "m", "windowId": "window-0",
                                    "sequence": 1, "ptsUs": 0}, frames, build_window(0))
    assert result["motion"]["inputProvenance"] == "simulated-input"
    assert len(result["motion"]["telemetry"]) == 3
    assert result["motion"]["estimates"] == []
    assert result["motion"]["oracle"]["worldSpeedMmps"] == 6000
    assert all("worldSpeedMmps" not in frame for frame in result["frames"])
    next_frames = [(dict(cameraId=camera, sequence=2, ptsUs=1_000_000), encoded.tobytes())
                   for camera in ("UAV1", "UAV2", "UAV3")]
    next_result = engine.process_window(
        {"runId": "r", "missionId": "m", "windowId": "window-1",
         "sequence": 2, "ptsUs": 1_000_000}, next_frames, build_window(1))
    assert next_result["motion"]["estimates"]
    assert all(item["status"] == "ok" for item in next_result["motion"]["estimates"])
    assert all(item["vehicleSpeedMmps"] == 0 for item in next_result["motion"]["estimates"])
    summary = summarize_motion_result(result)
    assert summary["status"] == "unknown"
    assert summary["oracle"]["provenance"] == "simulated-input"
    engine.close()


def test_fixture_writer_records_digest_bound_windows(tmp_path: Path) -> None:
    manifest = write_fixture(tmp_path / "motion", 3)
    assert manifest["provenance"] == "simulated-input"
    for window, path in manifest["windows"].items():
        payload = Path(path).read_text(encoding="utf-8").strip()
        assert window in path
        assert manifest["digests"][window].startswith("sha256:")
        assert json.loads(payload)["schema"] == "spec191-motion-window-v1"


def test_display_frame_keeps_motion_provenance() -> None:
    frame = DisplayFrame("r", "m", 1, 1, "UAV1", 2, 1_000_000, b"jpeg",
                         motion_status="estimated", speed_mmps=(6000,))
    restored = DisplayFrame.from_dict(frame.to_dict())
    assert restored.motion_status == "estimated"
    assert restored.speed_mmps == (6000,)
