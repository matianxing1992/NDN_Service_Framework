"""Analytic Spec210 geometry and source-bound worker integration tests."""

from __future__ import annotations

from dataclasses import replace
import copy
import csv
import hashlib
import json
import math
from pathlib import Path
import sys
import time

import cv2
import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))

from observation_schema import (FrameObservation, load_verified_observation_bundle)  # noqa: E402
from motion_estimator import GroundEstimate, MotionEstimator, project_ground_point  # noqa: E402
from observation_source import load_demo_profile  # noqa: E402
from tracking_display import DisplayFrame  # noqa: E402
from tracking_worker import TrackingEngine, _write_motion_csv  # noqa: E402


def _observation(camera: str, sequence: int, capture_time_us: int,
                 camera_x: float, image_digest: str, *, epoch: int = 7,
                 camera_z: float = 10.0,
                 focal_length: float = 100.0,
                 q_wc: tuple[float, float, float, float] = (0.0, 1.0, 0.0, 0.0),
                 acquisition_kind: str = "simulated",
                 delivery_mode: str = "replay"
                 ) -> FrameObservation:
    zero9 = [0.0] * 81
    zero6 = [0.0] * 36
    zero4 = [0.0] * 16
    raw = {
        "schema": "uav-observation-v2",
        "identity": {
            "sourceId": f"source-{camera}", "droneId": camera, "cameraId": camera,
            "epoch": epoch, "worldFrameId": "ENU-local",
            "acquisitionKind": acquisition_kind, "deliveryMode": delivery_mode,
        },
        "key": {"sourceId": f"source-{camera}", "cameraId": camera,
                "epoch": epoch, "sequence": sequence},
        "captureTimeUs": capture_time_us, "timeUncertaintyUs": 0,
        "width": 100, "height": 100,
        "image": {"encoding": "image/jpeg", "contentSha256": image_digest},
        "clockBinding": {
            "revision": 1, "clockId": f"clock-{camera}", "clockEpoch": epoch,
            "a": 1.0, "bUs": 0.0, "validFromUs": 0,
            "validUntilUs": 9_000_000_000, "uncertaintyUs": 0,
        },
        "calibration": {
            "schema": "uav-calibration-v2", "cameraId": camera, "revision": 1,
            "width": 100, "height": 100, "fx": focal_length, "fy": focal_length,
            "cx": 50.0, "cy": 50.0, "distortion": [0.0] * 5,
            "imageTransform": "raw", "validFromUs": 0, "validUntilUs": 9_000_000_000,
            "anchorSigmaPixels": 0.0, "parameterCovariance": zero9,
        },
        "ground": {
            "schema": "uav-ground-v2", "revision": 1,
            "worldFrameId": "ENU-local", "normal": [0.0, 0.0, 1.0],
            "offsetM": 0.0,
            "roiVertices": [[-100.0, -100.0, 0.0], [100.0, -100.0, 0.0],
                            [100.0, 100.0, 0.0], [-100.0, 100.0, 0.0]],
            "geometryValidity": "calibrated-plane", "parameterCovariance": zero4,
        },
        "poseStatus": "aligned", "positionW": [camera_x, 0.0, camera_z],
        # A 180-degree roll points the camera optical axis down into ENU.
        "qWC": list(q_wc), "poseCovariance": zero6,
        "anchorSigmaPixels": 0.0, "geometryValidity": "calibrated-plane", "reason": "",
    }
    return FrameObservation.parse(raw)


def _run_three_windows(*, moving_with_camera: bool,
                       samples: tuple[tuple[int, float], ...] =
                       ((0, 0.0), (500_000, 0.5), (1_000_000, 1.0)),
                       camera_z_samples: tuple[float, ...] | None = None,
                       q_wc_samples: tuple[tuple[float, float, float, float], ...] | None = None,
                       focal_length: float = 100.0,
                       anchor_samples: tuple[tuple[float, float], ...] | None = None,
                       sequence_samples: tuple[int, ...] | None = None
                       ) -> list[dict]:
    sample_count = len(samples)
    if any(values is not None and len(values) != sample_count
           for values in (camera_z_samples, q_wc_samples, anchor_samples, sequence_samples)):
        raise ValueError("pose and anchor samples must align with capture samples")
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    worker = TrackingEngine(functional=True)
    anchors: dict[str, tuple[int, int]] = {}

    def detect(_camera: str, _image: np.ndarray):
        x, y = anchors[_camera]
        return [((x - 5, y - 10, x + 5, y), 0.99, "car", None)]

    worker._detect = detect  # type: ignore[method-assign]
    results = []
    try:
        for sample_index, (time_us, camera_x) in enumerate(samples):
            sequence = sequence_samples[sample_index] if sequence_samples is not None else sample_index + 1
            camera_z = (camera_z_samples[sample_index]
                        if camera_z_samples is not None else 10.0)
            q_wc = (q_wc_samples[sample_index]
                    if q_wc_samples is not None else (0.0, 1.0, 0.0, 0.0))
            if anchor_samples is not None:
                anchor = tuple(round(value) for value in anchor_samples[sample_index])
                anchors.update({camera: anchor for camera in ("UAV1", "UAV2", "UAV3")})
            elif moving_with_camera:
                anchors.update({camera: (70, 50) for camera in ("UAV1", "UAV2", "UAV3")})
            else:
                # A stationary world point at x=3m moves left in the image as
                # the camera translates east, and projects back to x=3m.
                anchor = (round(50 + focal_length * (3.0 - camera_x) / camera_z), 50)
                anchors.update({camera: anchor for camera in ("UAV1", "UAV2", "UAV3")})
            frames = []
            observations = {}
            for camera in ("UAV1", "UAV2", "UAV3"):
                frames.append(({"cameraId": camera, "sequence": sequence, "ptsUs": 0}, payload))
                observations[camera] = _observation(
                    camera, sequence, time_us, camera_x, digest,
                    camera_z=camera_z, focal_length=focal_length, q_wc=q_wc)
            result = worker.process_window(
                {"runId": "analytic", "missionId": "m", "windowId": f"window-{sequence - 1}",
                 "sequence": sequence}, frames, observations=observations)
            results.append(result)
    finally:
        worker.close()
    return results


def test_moving_camera_keeps_stationary_vehicle_speed_near_zero() -> None:
    results = _run_three_windows(moving_with_camera=False)
    measurements = [next(item for item in result["motion"]["measurements"]
                         if item["frameKey"]["cameraId"] == "UAV1")
                    for result in results]
    assert [item["quality"] for item in measurements[:2]] == ["warming-up", "warming-up"]
    assert measurements[2]["quality"] == "stationary"
    assert measurements[2]["speedMps"] == pytest.approx(0.0, abs=1e-9)
    assert measurements[2]["courseDeg"] is None
    assert measurements[2]["horizontalRangeM"] == pytest.approx(2.0, abs=1e-6)
    assert measurements[2]["slantRangeM"] == pytest.approx((104.0) ** 0.5, abs=1e-6)
    assert measurements[2]["estimateTimeUs"] == 500_000
    assert measurements[2]["rangeTimeUs"] == 1_000_000
    assert measurements[2]["accuracyQualification"] == "unvalidated"


def test_same_pixel_vehicle_moving_with_camera_keeps_nonzero_world_speed(tmp_path: Path) -> None:
    results = _run_three_windows(moving_with_camera=True)
    measurements = [next(item for item in result["motion"]["measurements"]
                         if item["frameKey"]["cameraId"] == "UAV1")
                    for result in results]
    final = measurements[-1]
    assert final["speedMps"] == pytest.approx(1.0, abs=1e-6)
    assert final["courseDeg"] == pytest.approx(90.0, abs=1e-6)
    assert final["bearingStatus"] == "valid"
    assert final["frameKey"]["cameraId"] == "UAV1"
    assert final["trackKey"]["cameraId"] == "UAV1"
    assert final["anchorApproximation"] is True
    filename = _write_motion_csv(results[-1], tmp_path)
    with (tmp_path / filename).open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    row = next(item for item in rows if item["cameraId"] == "UAV1")
    assert row["anchorApproximation"] == "true"
    assert float(row["speedMps"]) == pytest.approx(1.0)
    assert row["localTrackId"] == str(final["trackKey"]["localTrackId"])
    assert row["observationDigest"] == final["observationRef"]["digest"]
    assert row["sequence"] == str(final["frameKey"]["sequence"])


@pytest.mark.parametrize("known_uncertainty", [False, True])
def test_worker_keeps_capture_measurements_for_100_frames_across_simulated_delivery_modes(
        monkeypatch: pytest.MonkeyPatch, known_uncertainty: bool) -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    modes = ("generated", "replay", "live-push")
    measurements_by_delivery: dict[tuple[str, float], dict[str, list[tuple[object, ...]]]] = {}

    for mode, rate in ((mode, rate) for mode in modes for rate in (0.5, 1.0, 2.0)):
        worker = TrackingEngine(functional=True)

        def detect(_camera: str, _image: np.ndarray):
            return [((65, 40, 75, 50), 0.99, "car", None)]

        worker._detect = detect  # type: ignore[method-assign]
        by_camera = {camera: [] for camera in ("UAV1", "UAV2", "UAV3")}
        try:
            for sequence in range(1, 101):
                capture_time_us = (sequence - 1) * 200_000
                camera_x = 3.0 * capture_time_us / 1_000_000.0
                frames = []
                observations = {}
                for camera in ("UAV1", "UAV2", "UAV3"):
                    frames.append(({
                        "cameraId": camera, "sequence": sequence,
                        "ptsUs": capture_time_us,
                    }, payload))
                    raw = _observation(
                        camera, sequence, capture_time_us, camera_x, digest,
                        delivery_mode=mode).raw
                    # Exercise both unknown uncertainty and noiseless analytic
                    # covariance. Neither asserts real sensor accuracy.
                    if not known_uncertainty:
                        raw["anchorSigmaPixels"] = None
                        raw["poseCovariance"] = None
                        raw["calibration"]["anchorSigmaPixels"] = None
                        raw["calibration"]["parameterCovariance"] = None
                        raw["ground"]["parameterCovariance"] = None
                    observations[camera] = FrameObservation.parse(raw)

                # Advance delivery clocks at the chosen rate while capture time
                # and geometry remain fixed. No sleep or estimator truth input.
                arrival_s = 10_000.0 + capture_time_us / 1_000_000.0 / rate
                with monkeypatch.context() as clock:
                    clock.setattr(time, "time", lambda: 1_700_000_000.0 + arrival_s)
                    clock.setattr(time, "monotonic", lambda: arrival_s)
                    clock.setattr(time, "perf_counter", lambda: arrival_s)
                    result = worker.process_window(
                        {"runId": "mode-parity", "missionId": "m",
                         "windowId": f"window-{sequence - 1}", "sequence": sequence},
                        frames, observations=observations)
                for measurement in result["motion"]["measurements"]:
                    camera = measurement["frameKey"]["cameraId"]
                    by_camera[camera].append((
                        measurement["captureTimeUs"], measurement["estimateTimeUs"],
                        measurement["speedMps"], measurement["courseDeg"],
                        measurement["horizontalRangeM"], measurement["slantRangeM"],
                        measurement["speedStatus"], measurement["bearingStatus"],
                        measurement["rangeStatus"],
                        measurement["quality"], measurement["accuracyQualification"],
                    ))
        finally:
            worker.close()
        measurements_by_delivery[(mode, rate)] = by_camera

    reference = measurements_by_delivery[("generated", 1.0)]
    for delivery, samples_by_camera in measurements_by_delivery.items():
        for camera in ("UAV1", "UAV2", "UAV3"):
            assert samples_by_camera[camera] == reference[camera], (delivery, camera)

    for camera in ("UAV1", "UAV2", "UAV3"):
        samples = reference[camera]
        assert len(samples) == 100
        assert samples[0][0] == 0
        assert samples[-1][0] == 19_800_000
        assert samples[-1][2] == pytest.approx(3.0, abs=1e-6)
        if known_uncertainty:
            assert samples[-1][3] == pytest.approx(90.0, abs=1e-6)
        else:
            assert samples[-1][3] is None
        assert samples[-1][4] == pytest.approx(2.0, abs=1e-6)
        assert samples[-1][5] == pytest.approx(math.sqrt(104.0), abs=1e-6)
        assert samples[0][6:] == ("unknown", "unknown", "valid", "warming-up", "unvalidated")
        assert samples[-1][6:] == (
            "valid", "valid" if known_uncertainty else "unknown", "valid", "estimated", "unvalidated")


def test_course_arrow_is_projected_through_current_camera_pose() -> None:
    q_roll = (0.0, 1.0, 0.0, 0.0)
    yaw = (math.cos(math.pi / 4.0), 0.0, 0.0, math.sin(math.pi / 4.0))
    q_wc = _quaternion_multiply(yaw, q_roll)
    q_samples = (q_wc, q_wc, q_wc)
    anchors = tuple(_forward_pixel((east, 0.0, 0.0), (0.0, 0.0, 10.0), q_wc,
                                   focal_length=150.0)
                    for east in (0.0, 0.5, 1.0))
    results = _run_three_windows(
        moving_with_camera=False,
        samples=((0, 0.0), (500_000, 0.0), (1_000_000, 0.0)),
        q_wc_samples=q_samples, focal_length=150.0, anchor_samples=anchors)
    measurement = results[-1]["motion"]["measurements"][0]
    assert measurement["courseDeg"] == pytest.approx(90.0, abs=1e-6)
    arrow = measurement["imageArrow"]
    assert arrow is not None
    assert abs(arrow["directionPx"][0]) < 1e-6
    assert arrow["directionPx"][1] > 0.0


def test_same_pixel_vehicle_moving_with_camera_uses_actual_unequal_capture_intervals() -> None:
    results = _run_three_windows(
        moving_with_camera=True,
        samples=((0, 0.0), (250_000, 0.25), (1_000_000, 1.0)))
    final = results[-1]["motion"]["measurements"]
    assert {item["frameKey"]["cameraId"] for item in final} == {"UAV1", "UAV2", "UAV3"}
    assert len({(item["trackKey"]["cameraId"], item["trackKey"]["localTrackId"])
                for item in final}) == 3
    assert all(item["speedMps"] == pytest.approx(1.0, abs=1e-6) for item in final)


def test_surviving_capture_keys_use_real_time_across_a_declared_gap() -> None:
    results = _run_three_windows(
        moving_with_camera=True, sequence_samples=(1, 3, 4),
        samples=((1_000_000, 0.0), (1_400_000, 2.0), (1_800_000, 4.0)))
    for result, sequence in zip(results, (1, 3, 4)):
        assert {int(frame["sequence"]) for frame in result["frames"]} == {sequence}
        assert {int(item["frameKey"]["sequence"])
                for item in result["motion"]["measurements"]} == {sequence}
    final = results[-1]["motion"]["measurements"]
    assert len(final) == 3
    # The world target moves 2m per actual 0.4s interval, despite missing key2.
    assert all(item["speedMps"] == pytest.approx(5.0, abs=1e-6) for item in final)
    assert all(item["courseDeg"] == pytest.approx(90.0, abs=1e-6) for item in final)


def _quaternion_multiply(left: tuple[float, float, float, float],
                         right: tuple[float, float, float, float]
                         ) -> tuple[float, float, float, float]:
    w1, x1, y1, z1 = left
    w2, x2, y2, z2 = right
    return (w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2,
            w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2)


def _rotate_quaternion(q: tuple[float, float, float, float],
                       vector: tuple[float, float, float]
                       ) -> tuple[float, float, float]:
    w, x, y, z = q
    vx, vy, vz = vector
    uv = (y * vz - z * vy, z * vx - x * vz, x * vy - y * vx)
    uuv = (y * uv[2] - z * uv[1], z * uv[0] - x * uv[2], x * uv[1] - y * uv[0])
    return tuple(vector[i] + 2.0 * (w * uv[i] + uuv[i]) for i in range(3))


def _forward_pixel(target: tuple[float, float, float],
                   camera: tuple[float, float, float],
                   q_wc: tuple[float, float, float, float],
                   focal_length: float = 100.0,
                   principal_point: tuple[float, float] = (50.0, 50.0)
                   ) -> tuple[float, float]:
    """Independent pinhole forward projection for the analytic fixtures."""
    conjugate = (q_wc[0], -q_wc[1], -q_wc[2], -q_wc[3])
    camera_ray = _rotate_quaternion(
        conjugate, tuple(target[index] - camera[index] for index in range(3)))
    assert camera_ray[2] > 0
    return (principal_point[0] + focal_length * camera_ray[0] / camera_ray[2],
            principal_point[1] + focal_length * camera_ray[1] / camera_ray[2])


def _wide_analytic_observation(camera_id: str, sequence: int, capture_time_us: int,
                               camera: tuple[float, float, float],
                               q_wc: tuple[float, float, float, float], *,
                               epoch: int = 7) -> dict:
    """Build a parsed, exact-geometry observation for multi-second oracles."""
    digest = "sha256:" + "0" * 64
    parsed = _observation(camera_id, sequence, capture_time_us, camera[0], digest,
                          epoch=epoch, camera_z=camera[2], focal_length=1000.0,
                          q_wc=q_wc)
    raw = copy.deepcopy(parsed.raw)
    raw["width"], raw["height"] = 2000, 1000
    raw["calibration"].update({"width": 2000, "height": 1000, "fx": 1000.0,
                               "fy": 1000.0, "cx": 1000.0, "cy": 500.0})
    raw["positionW"] = list(camera)
    raw["qWC"] = list(q_wc)
    raw["ground"]["roiVertices"] = [
        [-250.0, -250.0, 0.0], [250.0, -250.0, 0.0],
        [250.0, 250.0, 0.0], [-250.0, 250.0, 0.0],
    ]
    return FrameObservation.parse(raw).raw


def _project_and_update(estimator: MotionEstimator, track_key: tuple,
                        capture_time_us: int,
                        target: tuple[float, float, float],
                        camera: tuple[float, float, float],
                        q_wc: tuple[float, float, float, float]
                        ) -> tuple[dict, object, tuple[float, float]]:
    raw = _wide_analytic_observation(track_key[1], capture_time_us + 1,
                                     capture_time_us, camera, q_wc, epoch=track_key[2])
    pixel = _forward_pixel(target, camera, q_wc, focal_length=1000.0,
                           principal_point=(1000.0, 500.0))
    ground = project_ground_point(pixel, raw)
    return estimator.update(track_key, capture_time_us, ground), ground, pixel


def _oracle_ground_point(pixel: tuple[float, float], observation: dict) -> tuple[float, float, float]:
    """Independent zero-distortion pinhole ray/plane intersection oracle."""
    calibration, ground = observation["calibration"], observation["ground"]
    if any(float(value) != 0.0 for value in calibration["distortion"]):
        raise AssertionError("the covariance oracle is intentionally zero-distortion")
    ray_camera = ((pixel[0] - calibration["cx"]) / calibration["fx"],
                  (pixel[1] - calibration["cy"]) / calibration["fy"], 1.0)
    ray_norm = math.sqrt(sum(value * value for value in ray_camera))
    ray_camera = tuple(value / ray_norm for value in ray_camera)
    ray_world = _rotate_quaternion(tuple(observation["qWC"]), ray_camera)
    normal = tuple(ground["normal"])
    camera = tuple(observation["positionW"])
    offset = float(ground["offsetM"])
    distance = -(sum(normal[i] * camera[i] for i in range(3)) + offset) / \
        sum(normal[i] * ray_world[i] for i in range(3))
    return tuple(camera[i] + distance * ray_world[i] for i in range(3))


def _oracle_covariance(function, values: tuple[float, ...], covariance: np.ndarray,
                       steps: tuple[float, ...]) -> np.ndarray:
    jacobian = np.zeros((3, len(values)), dtype=np.float64)
    for index, step in enumerate(steps):
        if step <= 0 or not np.any(np.abs(covariance[:, index]) > 0):
            continue
        lower, upper = list(values), list(values)
        lower[index] -= step
        upper[index] += step
        jacobian[:, index] = (np.asarray(function(tuple(upper))) -
                              np.asarray(function(tuple(lower)))) / (2.0 * step)
    return jacobian @ covariance @ jacobian.T


def test_camera_rotation_about_optical_center_keeps_stationary_vehicle_still() -> None:
    samples = ((0, 0.0), (500_000, 0.0), (1_000_000, 0.0))
    q_roll = (0.0, 1.0, 0.0, 0.0)
    q_wc_samples = tuple(
        _quaternion_multiply(
            (math.cos(angle / 2.0), 0.0, 0.0, math.sin(angle / 2.0)), q_roll)
        for angle in (0.0, math.radians(10.0), math.radians(20.0)))
    anchors = tuple(_forward_pixel(
        (0.25, 0.0, 0.0), (0.0, 0.0, 10.0), q_wc, focal_length=2000.0)
                    for q_wc in q_wc_samples)
    results = _run_three_windows(
        moving_with_camera=False, samples=samples, q_wc_samples=q_wc_samples,
        focal_length=2000.0, anchor_samples=anchors)
    measurement = results[-1]["motion"]["measurements"][0]
    assert measurement["quality"] == "stationary"
    assert measurement["speedMps"] < 0.05


def test_camera_altitude_change_keeps_stationary_vehicle_still() -> None:
    samples = ((0, 0.0), (500_000, 0.0), (1_000_000, 0.0))
    altitudes = (8.0, 10.0, 12.0)
    anchors = tuple((50.0 + 100.0 * 3.0 / altitude, 50.0) for altitude in altitudes)
    results = _run_three_windows(
        moving_with_camera=False, samples=samples, camera_z_samples=altitudes,
        anchor_samples=anchors)
    measurement = results[-1]["motion"]["measurements"][0]
    assert measurement["quality"] == "stationary"
    assert measurement["speedMps"] < 0.05


def test_po04_analytic_ray_projection_and_range() -> None:
    observation = _observation("UAV1", 1, 0, 0.0, "sha256:" + "0" * 64)
    raw = copy.deepcopy(observation.raw)
    raw["width"], raw["height"] = 2000, 1000
    raw["calibration"].update({"width": 2000, "height": 1000, "fx": 1000.0,
                               "fy": 1000.0, "cx": 1000.0, "cy": 500.0})
    raw["positionW"] = [0.0, 0.0, 100.0]
    estimate = project_ground_point((1100.0, 500.0), raw)
    assert estimate.position_w == pytest.approx((10.0, 0.0, 0.0), abs=1e-9)
    assert estimate.horizontal_range_m == pytest.approx(10.0, abs=1e-9)
    assert estimate.slant_range_m == pytest.approx((10_100.0) ** 0.5, abs=1e-9)


def test_projection_reports_unknown_uncertainty_and_rejects_invalid_rays() -> None:
    observation = _observation("UAV1", 1, 0, 0.0, "sha256:" + "0" * 64)
    missing = copy.deepcopy(observation.raw)
    missing["anchorSigmaPixels"] = None
    missing["poseCovariance"] = None
    missing["calibration"]["parameterCovariance"] = None
    missing["ground"]["parameterCovariance"] = None
    estimate = project_ground_point((50.0, 50.0), missing)
    assert estimate.uncertainty_kind == "unknown"
    assert estimate.range_sigma_m is None

    for quaternion in ([1.0, 0.0, 0.0, 0.0],
                       [2.0 ** -0.5, 2.0 ** -0.5, 0.0, 0.0],
                       [2.0, 0.0, 0.0, 0.0],
                       [float("nan"), 0.0, 0.0, 0.0]):
        invalid = copy.deepcopy(observation.raw)
        invalid["qWC"] = quaternion
        invalid["positionW"] = [0.0, 0.0, 10.0]
        with pytest.raises(ValueError):
            project_ground_point((50.0, 50.0), invalid)

    outside = copy.deepcopy(observation.raw)
    outside["ground"]["roiVertices"] = [[-1.0, -1.0, 0.0], [1.0, -1.0, 0.0],
                                          [1.0, 1.0, 0.0], [-1.0, 1.0, 0.0]]
    with pytest.raises(ValueError, match="OUTSIDE_ROI"):
        project_ground_point((70.0, 50.0), outside)

    non_psd = copy.deepcopy(observation.raw)
    non_psd["poseCovariance"][0] = -1.0
    uncertain = project_ground_point((50.0, 50.0), non_psd)
    assert uncertain.uncertainty_kind == "unknown"
    assert uncertain.range_sigma_m is None


def test_geometry_validity_rejects_undeclared_values() -> None:
    observation = _observation("UAV1", 1, 0, 0.0, "sha256:" + "0" * 64)
    invalid = copy.deepcopy(observation.raw)
    invalid["geometryValidity"] = "valid"
    with pytest.raises(ValueError, match="geometryValidity"):
        FrameObservation.parse(invalid)


def test_worker_turns_invalid_geometry_into_unknown_measurements(tmp_path: Path) -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    worker = TrackingEngine(functional=True)
    worker._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    observations = {
        camera: _observation(camera, 1, 0, 0.0, digest)
        for camera in ("UAV1", "UAV2", "UAV3")
    }
    for observation in observations.values():
        observation.raw["ground"]["normal"] = [0.0, 0.0, 2.0]
    frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
              for camera in ("UAV1", "UAV2", "UAV3")]
    try:
        result = worker.process_window(
            {"runId": "invalid-geometry", "missionId": "m", "windowId": "w1",
             "sequence": 1}, frames, observations=observations)
    finally:
        worker.close()
    measurements = result["motion"]["measurements"]
    assert len(measurements) == 3
    assert all(item["quality"] == "unknown" for item in measurements)
    assert all(item["reasonCodes"] == ["INVALID_GROUND_NORMAL"] for item in measurements)
    assert all(item["estimateTimeUs"] is None for item in measurements)
    assert all(item["accuracyQualification"] == "unvalidated" for item in measurements)
    assert result["motion"]["accuracyQualification"] == "unvalidated"
    assert all(item["anchorApproximation"] is True for item in measurements)
    assert all(item["observation"]["anchorApproximation"] is True for item in result["frames"])
    for frame in result["frames"]:
        raw = observations[frame["cameraId"]].raw
        assert frame["observation"]["identity"] == raw["identity"]
        assert frame["observation"]["clockBinding"] == raw["clockBinding"]
        assert frame["observation"]["timeUncertaintyUs"] == raw["timeUncertaintyUs"]
    filename = _write_motion_csv(result, tmp_path)
    with (tmp_path / filename).open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 3
    assert all(row["anchorApproximation"] == "true" for row in rows)
    assert all(row["speedMps"] == "" for row in rows)


def test_analytic_fixture_uses_allowed_accuracy_qualification() -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    worker = TrackingEngine(functional=True)
    worker._detect = lambda _camera, _image: [((45, 40, 55, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    observations = {camera: _observation(camera, 1, 0, 0.0, digest)
                    for camera in ("UAV1", "UAV2", "UAV3")}
    for observation in observations.values():
        observation.raw["geometryValidity"] = "analytic-fixture"
        observation.raw["ground"]["geometryValidity"] = "analytic-fixture"
    frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
              for camera in ("UAV1", "UAV2", "UAV3")]
    try:
        result = worker.process_window(
            {"runId": "analytic-geometry", "missionId": "m", "windowId": "w1",
             "sequence": 1}, frames, observations=observations)
    finally:
        worker.close()
    assert result["motion"]["accuracyQualification"] == "analytic-only"
    assert {item["accuracyQualification"] for item in result["motion"]["measurements"]} == {
        "analytic-only"}


def test_motion_estimator_keeps_one_second_of_5hz_samples_for_20_seconds() -> None:
    estimator = MotionEstimator()
    covariance = (0.0,) * 9
    track_key = ("source-UAV1", "UAV1", 7, 4, 4)
    result = {}
    for sequence in range(101):
        capture_time_us = sequence * 200_000
        east = capture_time_us / 1_000_000.0
        ground = GroundEstimate((east, 0.0, 0.0), (east, 0.0, 10.0),
                                0.0, 10.0, covariance, covariance,
                                "linearized-assumed-independent", 0.0)
        result = estimator.update(track_key, capture_time_us, ground)
    history = estimator.export_state()["tracks"][0]["samples"]
    assert result["speedMps"] == pytest.approx(1.0, abs=1e-9)
    assert result["estimateTimeUs"] == 19_500_000
    assert len(history) == 6
    assert history[0]["timeUs"] == 19_000_000
    assert history[-1]["timeUs"] == 20_000_000


def test_motion_outlier_is_rejected_and_only_reseeds_history() -> None:
    estimator = MotionEstimator()
    covariance = (0.0,) * 9
    track_key = ("source-UAV1", "UAV1", 7, 4, 4)
    result = {}
    for capture_time_us, east in ((0, 0.0), (500_000, 30.0), (1_000_000, 100.0)):
        ground = GroundEstimate((east, 0.0, 0.0), (east, 0.0, 10.0),
                                0.0, 10.0, covariance, covariance,
                                "linearized-assumed-independent", 0.0)
        result = estimator.update(track_key, capture_time_us, ground)
    assert result["quality"] == "unknown"
    assert result["reason"] == "OUTLIER"
    assert result["speedMps"] is None
    samples = estimator.export_state()["tracks"][0]["samples"]
    assert len(samples) == 1
    assert samples[0]["timeUs"] == 1_000_000


def test_worker_motion_csv_is_the_same_frame_bound_measurement(tmp_path: Path) -> None:
    result = {
        "sequence": 7,
        "motion": {"measurements": [{
            "schema": "uav-motion-result-v2",
            "trackKey": {"sourceId": "source-1", "cameraId": "UAV2",
                         "sourceEpoch": 8, "localTrackId": 4, "trackGeneration": 4},
            "frameKey": {"sourceId": "source-1", "cameraId": "UAV2",
                         "sourceEpoch": 8, "sequence": 7},
            "observationRef": {"name": "/uav/obs/7", "digest": "sha256:" + "a" * 64},
            "captureTimeUs": 1_400_000, "rangeTimeUs": 1_400_000,
            "speedMps": None, "courseDeg": None, "horizontalRangeM": 12.5,
            "quality": "warming-up", "reasonCodes": ["TRACK_WARMUP"],
            "accuracyQualification": "illustrative-unvalidated",
            "anchorApproximation": True,
        }]},
    }
    filename = _write_motion_csv(result, tmp_path)
    assert filename == "motion-measurements-000007.csv"
    with (tmp_path / filename).open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 1
    assert rows[0]["cameraId"] == "UAV2"
    assert rows[0]["sequence"] == "7"
    assert rows[0]["observationName"] == "/uav/obs/7"
    assert rows[0]["speedMps"] == ""
    assert rows[0]["courseDeg"] == ""
    assert rows[0]["reasonCodes"] == '["TRACK_WARMUP"]'
    assert rows[0]["anchorApproximation"] == "true"
    assert list(rows[0])[-1] == "anchorApproximation"


def test_worker_rejects_image_digest_mismatch_before_tracker_mutation() -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    worker = TrackingEngine(functional=True)
    frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
              for camera in ("UAV1", "UAV2", "UAV3")]
    observations = {camera: _observation(camera, 1, 0, 0.0, digest)
                    for camera in ("UAV1", "UAV2", "UAV3")}
    observations["UAV3"] = replace(
        observations["UAV3"], raw={**observations["UAV3"].raw,
                                   "image": {"encoding": "image/jpeg",
                                             "contentSha256": "sha256:" + "0" * 64}})
    try:
        with pytest.raises(ValueError, match="digest mismatch"):
            worker.process_window(
                {"runId": "r", "missionId": "m", "windowId": "w0", "sequence": 1},
                frames, observations=observations)
        assert all(not worker._last[camera] for camera in ("UAV1", "UAV2", "UAV3"))
    finally:
        worker.close()


def _write_verified_observation_bundle(tmp_path: Path, payload: bytes, *, sequence: int = 1,
                                       capture_time_us: int = 0) -> tuple[list[dict], Path]:
    image_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    entries = []
    for camera in ("UAV1", "UAV2", "UAV3"):
        producer = f"/example/uav/drone/{camera}"
        observation = _observation(camera, sequence, capture_time_us, 0.0, image_digest)
        local = observation.raw
        prefix = f"{producer}/UAV/OBS/{camera}/7/{sequence}"
        pose = {
            "schema": "uav-pose-v2",
            "identity": {
                "sourceId": f"source-{camera}", "droneId": camera, "cameraId": camera,
                "epoch": "7", "worldFrameId": "ENU-local",
                "acquisitionKind": "simulated", "deliveryMode": "replay",
            },
            "sampleSequence": str(sequence), "deviceTimeUs": str(capture_time_us),
            "clockBinding": local["clockBinding"],
            "positionW": local["positionW"], "qWC": local["qWC"],
            "calibrationRevision": "1", "poseCovariance": local["poseCovariance"],
        }

        def make_reference(category: str, name: str, value: dict) -> tuple[dict, bytes]:
            content = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
            ref = {"exactName": name,
                   "contentSha256": "sha256:" + hashlib.sha256(content).hexdigest(),
                   "producerIdentity": producer}
            return ({"category": category, **ref}, content)

        refs_and_payloads = [
            make_reference("CLOCK", f"{producer}/UAV/CLOCK/1/v=17", local["clockBinding"]),
            make_reference("CALIBRATION", f"{producer}/UAV/CALIBRATION/{camera}/1/v=23", local["calibration"]),
            make_reference("GROUND", f"{producer}/UAV/GROUND/1/v=42", local["ground"]),
            make_reference("POSE", f"{prefix}/POSE/v={sequence}", pose),
        ]
        refs = [item[0] for item in refs_and_payloads]
        wire = {
            "schema": "uav-observation-v2", "identity": local["identity"], "key": local["key"],
            "captureTimeUs": local["captureTimeUs"], "timeUncertaintyUs": local["timeUncertaintyUs"],
            "width": local["width"], "height": local["height"],
            "imageRef": {"exactName": f"{prefix}/FRAME/v=1", "contentSha256": image_digest,
                         "producerIdentity": producer},
            "clockRef": {key: value for key, value in refs[0].items() if key != "category"},
            "calibrationRef": {key: value for key, value in refs[1].items() if key != "category"},
            "groundRef": {key: value for key, value in refs[2].items() if key != "category"},
            "poseStatus": "aligned",
            "poseSampleRefs": [{key: value for key, value in refs[3].items() if key != "category"}],
            "positionW": local["positionW"], "qWC": local["qWC"],
            "poseCovariance": local["poseCovariance"],
            "geometryValidity": local["geometryValidity"], "reason": "",
        }
        sidecar = json.dumps(wire, sort_keys=True, separators=(",", ":")).encode()
        (tmp_path / f"{camera}.jpg").write_bytes(payload)
        (tmp_path / f"{camera}.observation.json").write_bytes(sidecar)
        manifest_refs = []
        for index, (ref, content) in enumerate(refs_and_payloads):
            category = ref["category"]
            category_index = sum(1 for previous in refs[:index] if previous["category"] == category)
            filename = f"{camera}.reference-{category}-{category_index}.json"
            (tmp_path / filename).write_bytes(content)
            manifest_refs.append({**ref, "fileName": filename})
        entries.append({
            "cameraId": camera, "sourceId": observation.source_id,
            "producerIdentity": producer, "imageName": f"{prefix}/FRAME/v=1",
            "imageDigest": image_digest, "observationName": f"{prefix}/OBSERVATION/v=1",
            "observationDigest": "sha256:" + hashlib.sha256(sidecar).hexdigest(),
            "references": manifest_refs,
        })
    manifest_path = tmp_path / "verified-closure.json"
    manifest_path.write_text(json.dumps({
        "schema": "uav-verified-inputs-v2", "sourceEpoch": "7", "entries": entries,
    }), encoding="utf-8")
    return entries, manifest_path


def test_verified_observation_bundle_binds_sidecar_and_image_bytes(tmp_path: Path) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    payload = bytes(encoded)
    entries, manifest_path = _write_verified_observation_bundle(tmp_path, payload)
    observations = load_verified_observation_bundle(tmp_path, manifest_path)
    assert set(observations) == {"UAV1", "UAV2", "UAV3"}
    assert observations["UAV2"].sidecar_digest == entries[1]["observationDigest"]
    (tmp_path / "UAV2.observation.json").write_bytes(b"{}")
    with pytest.raises(ValueError, match="digest mismatch"):
        load_verified_observation_bundle(tmp_path, manifest_path)


def test_verified_observation_bundle_checks_complete_source_reference_closure(tmp_path: Path) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    _, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    observations = load_verified_observation_bundle(tmp_path, manifest_path)
    assert set(observations) == {"UAV1", "UAV2", "UAV3"}

    corrupted = tmp_path / "UAV2.reference-CLOCK-0.json"
    corrupted.write_bytes(corrupted.read_bytes() + b" ")
    with pytest.raises(ValueError, match="reference digest mismatch"):
        load_verified_observation_bundle(tmp_path, manifest_path)


def test_verified_observation_bundle_allows_external_calibration_provider(
        tmp_path: Path) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    entries, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entry = entries[1]
    producer = "/example/calibration-provider"
    exact_name = f"{producer}/UAV/CALIBRATION/UAV2/1/v=99"
    sidecar_path = tmp_path / "UAV2.observation.json"
    sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
    sidecar["calibrationRef"].update({
        "exactName": exact_name,
        "producerIdentity": producer,
    })
    encoded_sidecar = json.dumps(sidecar, sort_keys=True, separators=(",", ":")).encode()
    sidecar_path.write_bytes(encoded_sidecar)
    manifest_entry = manifest["entries"][1]
    manifest_entry["observationDigest"] = (
        "sha256:" + hashlib.sha256(encoded_sidecar).hexdigest())
    calibration_ref = next(
        reference for reference in manifest_entry["references"]
        if reference["category"] == "CALIBRATION")
    calibration_ref.update({"exactName": exact_name, "producerIdentity": producer})
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    observations = load_verified_observation_bundle(tmp_path, manifest_path)
    assert observations["UAV2"].source_id == entry["sourceId"]


def test_verified_observation_bundle_rejects_frame_scoped_static_reference_name(
        tmp_path: Path) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    entries, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entry = manifest["entries"][0]
    wrong_name = "/example/uav/drone/UAV1/UAV/OBS/UAV1/7/2/CLOCK/1/v=1"
    sidecar_path = tmp_path / "UAV1.observation.json"
    sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
    sidecar["clockRef"]["exactName"] = wrong_name
    encoded_sidecar = json.dumps(sidecar, sort_keys=True, separators=(",", ":")).encode()
    sidecar_path.write_bytes(encoded_sidecar)
    entry["observationDigest"] = "sha256:" + hashlib.sha256(encoded_sidecar).hexdigest()
    reference = next(item for item in entry["references"] if item["category"] == "CLOCK")
    reference["exactName"] = wrong_name
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError, match="invalid typed structure"):
        load_verified_observation_bundle(tmp_path, manifest_path)



def _replace_static_reference_name(root: Path, manifest_path: Path,
                                   category: str, exact_name: str) -> None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entry = manifest["entries"][0]
    sidecar_path = root / "UAV1.observation.json"
    sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
    field = {"CLOCK": "clockRef", "CALIBRATION": "calibrationRef", "GROUND": "groundRef"}[category]
    sidecar[field]["exactName"] = exact_name
    encoded = json.dumps(sidecar, sort_keys=True, separators=(",", ":")).encode()
    sidecar_path.write_bytes(encoded)
    entry["observationDigest"] = "sha256:" + hashlib.sha256(encoded).hexdigest()
    next(ref for ref in entry["references"] if ref["category"] == category)["exactName"] = exact_name
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")


@pytest.mark.parametrize("category", ["CLOCK", "CALIBRATION", "GROUND"])
@pytest.mark.parametrize("version", ["1", "87", "18446744073709551615"])
def test_static_reference_accepts_allocated_typed_versions(
        tmp_path: Path, category: str, version: str) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    _, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    category_path = f"{category}/UAV1" if category == "CALIBRATION" else category
    name = f"/example/uav/drone/UAV1/UAV/{category_path}/1/v={version}"
    _replace_static_reference_name(tmp_path, manifest_path, category, name)
    assert set(load_verified_observation_bundle(tmp_path, manifest_path)) == {"UAV1", "UAV2", "UAV3"}


@pytest.mark.parametrize("category", ["CLOCK", "CALIBRATION", "GROUND"])
@pytest.mark.parametrize("tail", [
    "0/v=17", "01/v=17", "18446744073709551616/v=17", "rev=1/v=17",
    "1/v=0", "1/v=017", "1/v=18446744073709551616", "1/17", "1/seq=17",
    "1/v=17/extra",
])
def test_static_reference_rejects_noncanonical_revision_and_version(
        tmp_path: Path, category: str, tail: str) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    _, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    category_path = f"{category}/UAV1" if category == "CALIBRATION" else category
    name = f"/example/uav/drone/UAV1/UAV/{category_path}/{tail}"
    _replace_static_reference_name(tmp_path, manifest_path, category, name)
    with pytest.raises(ValueError, match="invalid typed structure"):
        load_verified_observation_bundle(tmp_path, manifest_path)


@pytest.mark.parametrize("category", ["CLOCK", "CALIBRATION", "GROUND"])
def test_static_reference_rejects_digest_valid_name_payload_revision_mismatch(
        tmp_path: Path, category: str) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    _, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    category_path = f"{category}/UAV1" if category == "CALIBRATION" else category
    name = f"/example/uav/drone/UAV1/UAV/{category_path}/2/v=17"
    _replace_static_reference_name(tmp_path, manifest_path, category, name)
    # Existing payload declares revision 1; its manifest/sidecar digest is unchanged.
    # This failure is reached only after reading the record and verifying its digest.
    with pytest.raises(ValueError, match="exact name does not match its record identity"):
        load_verified_observation_bundle(tmp_path, manifest_path)


def test_static_references_remain_valid_across_distinct_frame_sequences(tmp_path: Path) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    entries, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded), sequence=1)
    load_verified_observation_bundle(tmp_path, manifest_path)
    originals = {entry["cameraId"]: [ref["exactName"] for ref in entry["references"][:3]]
                 for entry in entries}
    next_entries, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded), sequence=2)
    load_verified_observation_bundle(tmp_path, manifest_path)
    for previous, current in zip(entries, next_entries):
        assert previous["imageName"] != current["imageName"]
        assert previous["observationName"] != current["observationName"]
        assert originals[current["cameraId"]] == [ref["exactName"] for ref in current["references"][:3]]


def test_verified_observation_wire_rejects_inline_payloads_and_unbound_pose_estimates(
        tmp_path: Path) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    _, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    sidecar_path = tmp_path / "UAV1.observation.json"

    wire = json.loads(sidecar_path.read_text(encoding="utf-8"))
    wire["calibration"] = {"inline": "forbidden"}
    sidecar = json.dumps(wire, sort_keys=True, separators=(",", ":")).encode()
    sidecar_path.write_bytes(sidecar)
    manifest["entries"][0]["observationDigest"] = "sha256:" + hashlib.sha256(sidecar).hexdigest()
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError, match="unknown fields"):
        load_verified_observation_bundle(tmp_path, manifest_path)

    del wire["calibration"]
    wire["positionW"][0] += 1.0
    sidecar = json.dumps(wire, sort_keys=True, separators=(",", ":")).encode()
    sidecar_path.write_bytes(sidecar)
    manifest["entries"][0]["observationDigest"] = "sha256:" + hashlib.sha256(sidecar).hexdigest()
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError, match="positionW does not match"):
        load_verified_observation_bundle(tmp_path, manifest_path)


def test_source_identity_strings_are_bounded_in_utf8_bytes() -> None:
    raw = copy.deepcopy(
        _observation("UAV1", 1, 0, 0.0, "sha256:" + "0" * 64).raw)
    raw["identity"]["droneId"] = "d" * 129
    with pytest.raises(ValueError, match="droneId exceeds 128 UTF-8 bytes"):
        FrameObservation.parse(raw)


def test_epoch_change_starts_new_motion_warmup() -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    worker = TrackingEngine(functional=True)
    worker._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    try:
        for sequence in (1, 2):
            frames = [({"cameraId": camera, "sequence": sequence, "ptsUs": 0}, payload)
                      for camera in ("UAV1", "UAV2", "UAV3")]
            observations = {camera: _observation(camera, sequence, sequence * 500_000,
                                                   sequence * 0.5, digest)
                            for camera in ("UAV1", "UAV2", "UAV3")}
            worker.process_window(
                {"runId": "r", "missionId": "m", "windowId": f"w{sequence}",
                 "sequence": sequence}, frames, observations=observations)

        frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
                  for camera in ("UAV1", "UAV2", "UAV3")]
        observations = {camera: _observation(camera, 1, 0, 0.0, digest, epoch=8)
                        for camera in ("UAV1", "UAV2", "UAV3")}
        result = worker.process_window(
            {"runId": "r", "missionId": "m", "windowId": "new-source",
             "sequence": 1}, frames, observations=observations)
        measurement = next(item for item in result["motion"]["measurements"]
                           if item["frameKey"]["cameraId"] == "UAV1")
        assert measurement["trackKey"]["sourceEpoch"] == 8
        assert measurement["quality"] == "warming-up"
    finally:
        worker.close()


def test_observation_motion_history_survives_verified_worker_restart(tmp_path: Path) -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    state = tmp_path / "tracking-state.json"
    first = TrackingEngine(functional=True)
    first._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    try:
        for sequence in (1, 2):
            frames = [({"cameraId": camera, "sequence": sequence, "ptsUs": 0}, payload)
                      for camera in ("UAV1", "UAV2", "UAV3")]
            observations = {camera: _observation(camera, sequence, (sequence - 1) * 500_000,
                                                   (sequence - 1) * 0.5, digest)
                            for camera in ("UAV1", "UAV2", "UAV3")}
            first.process_window(
                {"runId": "r", "missionId": "m", "windowId": f"w{sequence}",
                 "sequence": sequence}, frames, observations=observations)
        first.save_state(state)
    finally:
        first.close()

    resumed = TrackingEngine(functional=True)
    resumed._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    try:
        resumed.restore_state(state)
        frames = [({"cameraId": camera, "sequence": 3, "ptsUs": 0}, payload)
                  for camera in ("UAV1", "UAV2", "UAV3")]
        observations = {camera: _observation(camera, 3, 1_000_000, 1.0, digest)
                        for camera in ("UAV1", "UAV2", "UAV3")}
        result = resumed.process_window(
            {"runId": "r", "missionId": "m", "windowId": "w3", "sequence": 3},
            frames, observations=observations)
        measurement = next(item for item in result["motion"]["measurements"]
                           if item["frameKey"]["cameraId"] == "UAV1")
        assert measurement["speedMps"] == pytest.approx(1.0, abs=1e-6)
        assert measurement["quality"] == "estimated"
        for frame in result["frames"]:
            raw = observations[frame["cameraId"]].raw
            assert frame["observation"]["identity"] == raw["identity"]
            assert frame["observation"]["clockBinding"] == raw["clockBinding"]
            assert frame["observation"]["timeUncertaintyUs"] == raw["timeUncertaintyUs"]
            assert frame["observation"]["anchorApproximation"] is True
    finally:
        resumed.close()


def test_same_epoch_calibration_change_resets_history_and_records_reason() -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    worker = TrackingEngine(functional=True)
    worker._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    try:
        first_observations = {
            camera: _observation(camera, 1, 0, 0.0, digest)
            for camera in ("UAV1", "UAV2", "UAV3")
        }
        first_frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
                        for camera in ("UAV1", "UAV2", "UAV3")]
        worker.process_window({"runId": "r", "missionId": "m", "windowId": "window-0",
                               "sequence": 1}, first_frames,
                             observations=first_observations)

        changed_observations = {}
        for camera in ("UAV1", "UAV2", "UAV3"):
            raw = copy.deepcopy(_observation(camera, 2, 500_000, 0.5, digest).raw)
            raw["calibration"]["revision"] = 2
            changed_observations[camera] = FrameObservation.parse(raw)
        second_frames = [({"cameraId": camera, "sequence": 2, "ptsUs": 0}, payload)
                         for camera in ("UAV1", "UAV2", "UAV3")]
        result = worker.process_window(
            {"runId": "r", "missionId": "m", "windowId": "window-1", "sequence": 2},
            second_frames, observations=changed_observations)

        measurement = next(item for item in result["motion"]["measurements"]
                           if item["frameKey"]["cameraId"] == "UAV1")
        assert measurement["quality"] == "warming-up"
        assert result["stateResetReason"] == (
            "STATE_BINDING_MISMATCH:sources.UAV1.calibrationDigest")
    finally:
        worker.close()


def test_malformed_session_restore_does_not_partially_mutate_worker(tmp_path: Path) -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    observations = {
        camera: _observation(camera, 1, 0, 0.0, digest)
        for camera in ("UAV1", "UAV2", "UAV3")
    }
    source = TrackingEngine(functional=True)
    source._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    state_path = tmp_path / "valid-state.json"
    try:
        frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
                  for camera in ("UAV1", "UAV2", "UAV3")]
        source.process_window({"runId": "r", "missionId": "m", "windowId": "window-0",
                               "sequence": 1}, frames, observations=observations)
        source.save_state(state_path)
    finally:
        source.close()

    malformed = json.loads(state_path.read_text(encoding="utf-8"))
    malformed["motionEstimator"]["policy"]["minimumPoints"] += 1
    malformed_path = tmp_path / "malformed-state.json"
    malformed_path.write_text(json.dumps(malformed), encoding="utf-8")
    resumed = TrackingEngine(functional=True)
    resumed._next_local = {camera: 23 for camera in ("UAV1", "UAV2", "UAV3")}
    before = dict(resumed._next_local)
    try:
        with pytest.raises(ValueError, match="motion state policy does not match"):
            resumed.restore_state(malformed_path, malformed["sessionBinding"])
        assert resumed._next_local == before
        assert resumed._session_binding is None
        assert resumed._last_observation_sequence is None
    finally:
        resumed.close()


@pytest.mark.parametrize("legacy_schema", [
    "spec191-tracking-session-state-v1",
    "spec191-tracking-session-state-v2",
])
def test_legacy_session_state_cannot_seed_spec210_history(
        tmp_path: Path, legacy_schema: str) -> None:
    image = np.zeros((100, 100, 3), dtype=np.uint8)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    payload = bytes(encoded)
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    legacy = TrackingEngine(functional=True)
    legacy._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    legacy_state = tmp_path / "legacy-state.json"
    try:
        frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
                  for camera in ("UAV1", "UAV2", "UAV3")]
        first = legacy.process_window({"runId": "r", "missionId": "m", "windowId": "legacy-0",
                                       "sequence": 1}, frames)
        legacy.save_state(legacy_state)
    finally:
        legacy.close()

    saved = json.loads(legacy_state.read_text(encoding="utf-8"))
    legacy_state_fields = ["cameras", "nextLocal", "last", "associator"]
    if legacy_schema == "spec191-tracking-session-state-v2":
        legacy_state_fields.append("byteTrack")
    compatible_state = {key: saved[key] for key in legacy_state_fields}
    compatible_state["schema"] = legacy_schema
    legacy_state.write_text(json.dumps(compatible_state), encoding="utf-8")

    legacy_resumed = TrackingEngine(functional=True)
    legacy_resumed._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    try:
        legacy_resumed.restore_state(legacy_state)
        next_frames = [({"cameraId": camera, "sequence": 2, "ptsUs": 1_000_000}, payload)
                       for camera in ("UAV1", "UAV2", "UAV3")]
        continued = legacy_resumed.process_window(
            {"runId": "r", "missionId": "m", "windowId": "legacy-1", "sequence": 2},
            next_frames)
        assert [track["localId"] for frame in continued["frames"]
                for track in frame["tracks"]] == [
                    track["localId"] for frame in first["frames"] for track in frame["tracks"]]
    finally:
        legacy_resumed.close()

    resumed = TrackingEngine(functional=True)
    resumed._detect = lambda _camera, _image: [((65, 40, 75, 50), 0.99, "car", None)]  # type: ignore[method-assign]
    try:
        observations = {
            camera: _observation(camera, 1, 0, 0.0, digest)
            for camera in ("UAV1", "UAV2", "UAV3")
        }
        expected_binding = resumed._observation_session_binding(
            {"runId": "r", "missionId": "m"}, observations)
        resumed.restore_state(legacy_state, expected_binding)
        frames = [({"cameraId": camera, "sequence": 1, "ptsUs": 0}, payload)
                  for camera in ("UAV1", "UAV2", "UAV3")]
        result = resumed.process_window(
            {"runId": "r", "missionId": "m", "windowId": "window-0", "sequence": 1},
            frames, observations=observations)
        assert result["stateResetReason"] == "STATE_SCHEMA_MISMATCH:legacy-to-observation"
        assert all(track["localId"] == 1 for frame in result["frames"] for track in frame["tracks"])
        assert all(item["quality"] == "warming-up" for item in result["motion"]["measurements"])
    finally:
        resumed.close()


def test_display_protocol_preserves_per_camera_measurement_and_qualification() -> None:
    measurement = {
        "schema": "uav-motion-result-v2", "trackKey": {"cameraId": "UAV2"},
        "speedMps": 1.25, "courseDeg": 90.0, "quality": "estimated",
    }
    frame = DisplayFrame(
        "r", "m", 7, 1, "UAV2", 3, 1_000_000, b"jpeg",
        tracks=({"localId": 4, "measurement": measurement},),
        measurements=(measurement,), provenance="simulated/replay",
        accuracy_qualification="illustrative-unvalidated")
    restored = DisplayFrame.from_dict(frame.to_dict())
    assert restored.measurements[0]["trackKey"]["cameraId"] == "UAV2"
    assert restored.provenance == "simulated/replay"
    assert restored.accuracy_qualification == "illustrative-unvalidated"


@pytest.mark.parametrize("scenario", [
    "stationary-camera", "moving-camera-static-car", "co-moving",
    "camera-rotation", "camera-altitude",
])
def test_po05_named_twenty_second_geometry_scenarios_meet_sc003(scenario: str) -> None:
    estimator = MotionEstimator()
    track_key = ("source-UAV1", "UAV1", 7, 1, 1)
    q_down = (0.0, 1.0, 0.0, 0.0)
    worst_position_error = 0.0
    worst_range_error = 0.0
    results = []

    for sequence in range(101):
        time_us = sequence * 200_000
        seconds = time_us / 1_000_000.0
        if scenario == "stationary-camera":
            camera, target, q_wc = (0.0, 0.0, 300.0), (10.0 * seconds, 0.0, 0.0), q_down
        elif scenario == "moving-camera-static-car":
            camera, target, q_wc = (-50.0 + 5.0 * seconds, 0.0, 300.0), (0.0, 0.0, 0.0), q_down
        elif scenario == "co-moving":
            camera = (-50.0 + 10.0 * seconds, 0.0, 300.0)
            target, q_wc = (10.0 * seconds, 0.0, 0.0), q_down
        elif scenario == "camera-rotation":
            angle = 0.04 * seconds
            yaw = (math.cos(angle / 2.0), 0.0, 0.0, math.sin(angle / 2.0))
            camera, target = (0.0, 0.0, 300.0), (50.0, 20.0, 0.0)
            q_wc = _quaternion_multiply(yaw, q_down)
        else:
            camera = (0.0, 0.0, 250.0 + 5.0 * seconds)
            target, q_wc = (30.0, 20.0, 0.0), q_down

        result, ground, _pixel = _project_and_update(
            estimator, track_key, time_us, target, camera, q_wc)
        results.append(result)
        worst_position_error = max(worst_position_error, math.dist(ground.position_w, target))
        expected_horizontal = math.hypot(target[0] - camera[0], target[1] - camera[1])
        expected_slant = math.dist(target, camera)
        worst_range_error = max(
            worst_range_error,
            abs(ground.horizontal_range_m - expected_horizontal),
            abs(ground.slant_range_m - expected_slant),
        )

    assert len(results) == 101  # 5Hz including both endpoints of the 20s capture interval.
    assert worst_position_error <= 0.02
    assert worst_range_error <= 0.02
    final = results[-1]
    assert final["estimateTimeUs"] == 19_500_000
    if scenario in {"stationary-camera", "co-moving"}:
        assert final["speedMps"] == pytest.approx(10.0, abs=0.02)
        assert final["bearingDegreesTrue"] == pytest.approx(90.0, abs=0.2)
        assert final["bearingStatus"] == "valid"
        assert final["quality"] == "estimated"
    else:
        assert final["speedMps"] == pytest.approx(0.0, abs=0.05)
        assert final["bearingDegreesTrue"] is None
        assert final["bearingStatus"] == "unknown"
        assert final["quality"] == "stationary"


def test_po05_asynchronous_capture_times_preserve_motion_over_twenty_seconds() -> None:
    intervals_us = (100_000, 200_000, 300_000)
    times = [0]
    for index in range(101):
        times.append(times[-1] + intervals_us[index % len(intervals_us)])
    estimator = MotionEstimator()
    key = ("source-UAV1", "UAV1", 7, 1, 1)
    results = []
    for time_us in times:
        seconds = time_us / 1_000_000.0
        result, ground, _pixel = _project_and_update(
            estimator, key, time_us, (10.0 * seconds, 0.0, 0.0),
            (0.0, 0.0, 300.0), (0.0, 1.0, 0.0, 0.0))
        results.append(result)
        assert math.dist(ground.position_w, (10.0 * seconds, 0.0, 0.0)) <= 0.02
    assert len(times) == 102 and times[-1] == 20_100_000
    final = results[-1]
    assert final["speedMps"] == pytest.approx(10.0, abs=0.02)
    assert final["bearingDegreesTrue"] == pytest.approx(90.0, abs=0.2)
    assert final["estimateTimeUs"] == (times[-6] + times[-1]) // 2


def test_po05_same_global_track_identity_keeps_per_camera_ranges_separate() -> None:
    estimator = MotionEstimator()
    cameras = {"UAV1": (-20.0, 0.0, 300.0),
               "UAV2": (-10.0, 0.0, 300.0),
               "UAV3": (5.0, 0.0, 300.0)}
    latest = {}
    for sequence, time_us in enumerate((0, 500_000, 1_000_000), start=1):
        for camera_id, camera in cameras.items():
            key = (f"source-{camera_id}", camera_id, 7, 1, 1)
            result, _ground, _pixel = _project_and_update(
                estimator, key, time_us, (0.0, 0.0, 0.0), camera,
                (0.0, 1.0, 0.0, 0.0))
            latest[camera_id] = result
    assert {camera: value["distanceToCamera"]["horizontalM"]
            for camera, value in latest.items()} == pytest.approx(
                {"UAV1": 20.0, "UAV2": 10.0, "UAV3": 5.0})
    assert {tuple(value["trackKey"]) for value in latest.values()} == {
        ("source-UAV1", "UAV1", 7, 1, 1),
        ("source-UAV2", "UAV2", 7, 1, 1),
        ("source-UAV3", "UAV3", 7, 1, 1),
    }
    assert all(value["quality"] == "stationary" for value in latest.values())


def test_po05_restart_occlusion_and_reused_local_id_restart_warmup() -> None:
    estimator = MotionEstimator()
    q_down = (0.0, 1.0, 0.0, 0.0)
    key = ("source-UAV1", "UAV1", 7, 1, 1)
    result = {}
    for time_us in (0, 200_000, 400_000, 600_000):
        seconds = time_us / 1_000_000.0
        result, _ground, _pixel = _project_and_update(
            estimator, key, time_us, (5.0 * seconds, 0.0, 0.0),
            (0.0, 0.0, 300.0), q_down)
    assert result["speedMps"] == pytest.approx(5.0, abs=0.02)

    result, _ground, _pixel = _project_and_update(
        estimator, key, 2_000_000, (10.0, 0.0, 0.0),
        (0.0, 0.0, 300.0), q_down)
    assert result["quality"] == "warming-up"
    assert result["speedMps"] is None
    assert result["reason"] == "INSUFFICIENT_TRACK_HISTORY"

    for time_us in (2_200_000, 2_400_000):
        result, _ground, _pixel = _project_and_update(
            estimator, key, time_us, (5.0 * time_us / 1_000_000.0, 0.0, 0.0),
            (0.0, 0.0, 300.0), q_down)
    assert result["speedMps"] == pytest.approx(5.0, abs=0.02)

    restarted = ("source-UAV1", "UAV1", 8, 1, 1)
    result, _ground, _pixel = _project_and_update(
        estimator, restarted, 2_600_000, (13.0, 0.0, 0.0),
        (0.0, 0.0, 300.0), q_down)
    assert result["quality"] == "warming-up"
    assert result["speedMps"] is None
    assert result["trackKey"][2] == 8

    new_generation = ("source-UAV1", "UAV1", 8, 1, 2)
    result, _ground, _pixel = _project_and_update(
        estimator, new_generation, 2_800_000, (14.0, 0.0, 0.0),
        (0.0, 0.0, 300.0), q_down)
    assert result["quality"] == "warming-up"
    assert result["speedMps"] is None


def test_po05_acceleration_and_turn_report_window_average_velocity() -> None:
    zero_covariance = (0.0,) * 9
    key = ("source-UAV1", "UAV1", 7, 1, 1)
    accelerated = MotionEstimator()
    acceleration = 0.5
    result = {}
    for sequence in range(101):
        time_us = sequence * 200_000
        seconds = time_us / 1_000_000.0
        position = (0.5 * acceleration * seconds * seconds, 0.0, 0.0)
        ground = GroundEstimate(position, (0.0, 0.0, 300.0),
                                math.hypot(position[0], position[1]),
                                math.dist(position, (0.0, 0.0, 300.0)),
                                zero_covariance, zero_covariance,
                                "linearized-assumed-independent", 0.0)
        result = accelerated.update(key, time_us, ground)
    assert result["estimateTimeUs"] == 19_500_000
    assert result["speedMps"] == pytest.approx(acceleration * 19.5, abs=1e-9)
    assert result["bearingDegreesTrue"] == pytest.approx(90.0, abs=1e-9)

    turning = MotionEstimator()
    radius, angular_rate = 40.0, 0.05
    samples = []
    for sequence in range(101):
        time_us = sequence * 200_000
        seconds = time_us / 1_000_000.0
        position = (radius * math.cos(angular_rate * seconds),
                    radius * math.sin(angular_rate * seconds), 0.0)
        ground = GroundEstimate(position, (0.0, 0.0, 300.0),
                                math.hypot(position[0], position[1]),
                                math.dist(position, (0.0, 0.0, 300.0)),
                                zero_covariance, zero_covariance,
                                "linearized-assumed-independent", 0.0)
        result = turning.update(key, time_us, ground)
        samples.append((seconds, position))
    window = samples[-6:]
    mean_time = sum(time for time, _ in window) / len(window)
    denominator = sum((time - mean_time) ** 2 for time, _ in window)
    weights = [(time - mean_time) / denominator for time, _ in window]
    expected_velocity = tuple(
        sum(weights[index] * window[index][1][axis] for index in range(len(window)))
        for axis in range(2))
    assert result["estimateTimeUs"] == 19_500_000
    assert result["velocityENU_mps"][:2] == pytest.approx(expected_velocity, abs=1e-9)
    assert result["speedMps"] == pytest.approx(math.hypot(*expected_velocity), abs=1e-9)
    assert result["speedMps"] == pytest.approx(2.0, abs=0.001)


def test_po06_projection_covariance_matches_independent_parameter_perturbations() -> None:
    raw = _wide_analytic_observation(
        "UAV1", 1, 1_000_000, (0.0, 0.0, 100.0), (0.0, 1.0, 0.0, 0.0))

    pose_covariance = np.diag([
        0.25 ** 2, 0.25 ** 2, 0.25 ** 2,
        math.radians(0.2) ** 2, math.radians(0.2) ** 2, math.radians(0.2) ** 2,
    ])
    pose_covariance[0, 2] = pose_covariance[2, 0] = 0.25 * 0.25 * 0.25
    pose_covariance[3, 4] = pose_covariance[4, 3] = 0.2 * math.radians(0.2) ** 2

    calibration_covariance = np.diag([100.0, 100.0, 1.0, 1.0, 0, 0, 0, 0, 0])
    calibration_covariance[0, 2] = calibration_covariance[2, 0] = 0.5
    calibration_covariance[1, 3] = calibration_covariance[3, 1] = -0.3

    ground_covariance = np.diag([1e-6, 1e-6, 0.0, 0.04])
    ground_covariance[0, 3] = ground_covariance[3, 0] = 5e-5
    ground_covariance[1, 3] = ground_covariance[3, 1] = -2e-5

    raw["poseCovariance"] = pose_covariance.reshape(-1).tolist()
    raw["calibration"]["parameterCovariance"] = calibration_covariance.reshape(-1).tolist()
    raw["ground"]["parameterCovariance"] = ground_covariance.reshape(-1).tolist()
    raw["anchorSigmaPixels"] = 2.0
    raw["calibration"]["anchorSigmaPixels"] = 2.0
    observation = FrameObservation.parse(raw)
    raw = observation.raw
    pixel = (1150.0, 420.0)
    estimate = project_ground_point(pixel, raw)

    pixel_covariance = np.eye(2) * 2.0 ** 2
    expected_pixel = _oracle_covariance(
        lambda value: _oracle_ground_point(value, raw), pixel, pixel_covariance,
        (0.25, 0.25))

    pose_values = tuple([*raw["positionW"], 0.0, 0.0, 0.0])

    def pose_projection(values: tuple[float, ...]) -> tuple[float, float, float]:
        edited = copy.deepcopy(raw)
        edited["positionW"] = list(values[:3])
        rotation = values[3:]
        magnitude = math.sqrt(sum(value * value for value in rotation))
        if magnitude <= 1e-15:
            delta_q = (1.0, 0.0, 0.0, 0.0)
        else:
            half_angle = magnitude / 2.0
            scale = math.sin(half_angle) / magnitude
            delta_q = (math.cos(half_angle), *(value * scale for value in rotation))
        edited["qWC"] = list(_quaternion_multiply(tuple(raw["qWC"]), delta_q))
        return _oracle_ground_point(pixel, edited)

    expected_pose = _oracle_covariance(
        pose_projection, pose_values, pose_covariance,
        (0.002, 0.002, 0.002, 1e-4, 1e-4, 1e-4))

    calibration = raw["calibration"]
    calibration_values = tuple([
        calibration["fx"], calibration["fy"], calibration["cx"], calibration["cy"],
        *calibration["distortion"],
    ])

    def calibration_projection(values: tuple[float, ...]) -> tuple[float, float, float]:
        edited = copy.deepcopy(raw)
        for index, name in enumerate(("fx", "fy", "cx", "cy")):
            edited["calibration"][name] = values[index]
        edited["calibration"]["distortion"] = list(values[4:])
        return _oracle_ground_point(pixel, edited)

    expected_calibration = _oracle_covariance(
        calibration_projection, calibration_values, calibration_covariance,
        (0.1, 0.1, 0.01, 0.01, 0.0, 0.0, 0.0, 0.0, 0.0))

    ground = raw["ground"]
    ground_values = tuple([*ground["normal"], ground["offsetM"]])

    def ground_projection(values: tuple[float, ...]) -> tuple[float, float, float]:
        edited = copy.deepcopy(raw)
        norm = math.sqrt(sum(value * value for value in values[:3]))
        edited["ground"]["normal"] = [value / norm for value in values[:3]]
        edited["ground"]["offsetM"] = values[3] / norm
        return _oracle_ground_point(pixel, edited)

    expected_ground = _oracle_covariance(
        ground_projection, ground_values, ground_covariance,
        (1e-5, 1e-5, 0.0, 1e-3))
    expected_system = expected_pose + expected_calibration + expected_ground
    np.testing.assert_allclose(
        np.asarray(estimate.pixel_covariance).reshape(3, 3), expected_pixel,
        rtol=3e-4, atol=1e-8)
    np.testing.assert_allclose(
        np.asarray(estimate.system_covariance).reshape(3, 3), expected_system,
        rtol=3e-4, atol=1e-8)
    assert estimate.uncertainty_kind == "linearized-assumed-independent"

    expected_point = _oracle_ground_point(pixel, raw)
    camera = np.asarray(raw["positionW"], dtype=np.float64)
    direction = np.asarray(expected_point) - camera
    direction /= np.linalg.norm(direction)
    total_covariance = expected_pixel + expected_system
    expected_range_sigma = math.sqrt(float(direction @ total_covariance @ direction))
    assert estimate.range_sigma_m == pytest.approx(expected_range_sigma, rel=3e-4, abs=1e-8)
    assert estimate.range_sigma_m is not None and estimate.range_sigma_m > 0


def test_po06_projection_accepts_wire_numeric_strings_in_pose_covariance_path() -> None:
    raw = _wide_analytic_observation(
        "UAV1", 1, 1_000_000, (0.0, 0.0, 100.0), (0.0, 1.0, 0.0, 0.0))
    # Native observations encode numeric fields as JSON number strings. The
    # schema accepts this wire representation, so geometry must normalize the
    # quaternion before applying finite-difference pose perturbations.
    raw["positionW"] = [str(value) for value in raw["positionW"]]
    raw["qWC"] = [str(value) for value in raw["qWC"]]
    raw["poseCovariance"] = [str(value) for value in [0.0] * 36]
    parsed = FrameObservation.parse(raw)

    estimate = project_ground_point((1150.0, 420.0), parsed.raw)

    assert estimate.position_w == pytest.approx((15.0, 8.0, 0.0), abs=1e-6)
    assert estimate.system_covariance is not None
    assert estimate.uncertainty_kind == "linearized-assumed-independent"


def test_po06_shared_system_error_is_not_averaged_as_pixel_noise() -> None:
    pixel_covariance = np.diag([0.04, 0.04, 0.04]).reshape(-1)
    system_covariance = np.diag([0.04, 0.04, 0.04]).reshape(-1)
    system_sigmas = []
    pixel_sigmas = []

    for sample_count in (6, 11, 21):
        estimator = MotionEstimator()
        key = ("source-UAV1", "UAV1", 7, 1, 1)
        latest = {}
        times = np.linspace(0.0, 1.0, sample_count)
        for time_value in times:
            time_us = round(time_value * 1_000_000)
            position = (10.0 * time_value, 0.0, 0.0)
            ground = GroundEstimate(
                position, (0.0, 0.0, 300.0), 10.0 * time_value,
                math.hypot(10.0 * time_value, 300.0),
                tuple(pixel_covariance), tuple(system_covariance),
                "linearized-assumed-independent", None)
            latest = estimator.update(key, time_us, ground)

        centered = times - np.mean(times)
        weights = centered / np.sum(centered ** 2)
        pixel_variance = float(np.sum(weights ** 2) * pixel_covariance[0])
        system_sigma = float(np.sum(np.abs(weights)) * math.sqrt(0.12))
        expected_sigma = math.sqrt(pixel_variance + system_sigma ** 2)
        assert latest["sigmaSpeedMps"] == pytest.approx(expected_sigma, rel=1e-10)
        assert latest["sigmaSpeedMps"] is not None
        system_sigmas.append(system_sigma)
        pixel_sigmas.append(math.sqrt(pixel_variance))

    assert system_sigmas[-1] > 0.9 * system_sigmas[0]
    assert pixel_sigmas[-1] < 0.7 * pixel_sigmas[0]


def test_po06_unknown_covariance_never_becomes_zero_direction_confidence() -> None:
    estimator = MotionEstimator()
    key = ("source-UAV1", "UAV1", 7, 1, 1)
    result = {}
    for time_us in (0, 200_000, 400_000):
        seconds = time_us / 1_000_000.0
        position = (0.2 * seconds, 0.0, 0.0)
        ground = GroundEstimate(position, (0.0, 0.0, 300.0),
                                0.2 * seconds, math.hypot(0.2 * seconds, 300.0),
                                None, None, "unknown", None)
        result = estimator.update(key, time_us, ground)
    assert result["speedMps"] == pytest.approx(0.2, abs=1e-9)
    assert result["sigmaSpeedMps"] is None
    assert result["bearingDegreesTrue"] is None
    assert result["bearingStatus"] == "unknown"
    assert result["quality"] == "degraded"
    assert result["reason"] == "LOW_SPEED_DIRECTION_UNKNOWN"


def test_po06_invalid_geometry_and_missing_uncertainty_fail_closed() -> None:
    base = _wide_analytic_observation(
        "UAV1", 1, 0, (0.0, 0.0, 100.0), (0.0, 1.0, 0.0, 0.0))
    pixel = (1000.0, 500.0)

    missing_intrinsics = copy.deepcopy(base)
    del missing_intrinsics["calibration"]["fx"]
    with pytest.raises(ValueError):
        FrameObservation.parse(missing_intrinsics)

    invalid_quaternion = copy.deepcopy(base)
    invalid_quaternion["qWC"] = [0.0, 0.0, 0.0, 0.0]
    with pytest.raises(ValueError, match="unit length"):
        FrameObservation.parse(invalid_quaternion)

    truth_in_production_record = copy.deepcopy(base)
    truth_in_production_record["groundTruth"] = {"positionW": [1.0, 2.0, 0.0]}
    with pytest.raises(ValueError, match="unknown"):
        FrameObservation.parse(truth_in_production_record)

    missing = copy.deepcopy(base)
    missing["anchorSigmaPixels"] = None
    missing["calibration"]["anchorSigmaPixels"] = None
    missing["poseCovariance"] = None
    missing["calibration"]["parameterCovariance"] = None
    missing["ground"]["parameterCovariance"] = None
    parsed_missing = FrameObservation.parse(missing)
    unknown = project_ground_point(pixel, parsed_missing.raw)
    assert unknown.uncertainty_kind == "unknown"
    assert unknown.pixel_covariance is None
    assert unknown.system_covariance is None
    assert unknown.range_sigma_m is None

    without_calibration = copy.deepcopy(base)
    without_calibration["calibration"] = None
    with pytest.raises(ValueError, match="CALIBRATION_OR_GROUND_MISSING"):
        project_ground_point(pixel, without_calibration)

    near_horizon = copy.deepcopy(base)
    angle = math.radians(85.0)
    pitch = (math.cos(angle / 2.0), 0.0, math.sin(angle / 2.0), 0.0)
    near_horizon["qWC"] = list(_quaternion_multiply(pitch, (0.0, 1.0, 0.0, 0.0)))
    with pytest.raises(ValueError, match="RAY_NEAR_HORIZON"):
        project_ground_point(pixel, near_horizon)

    ground_behind_camera = copy.deepcopy(base)
    ground_behind_camera["qWC"] = [1.0, 0.0, 0.0, 0.0]
    with pytest.raises(ValueError, match="GROUND_INTERSECTION_BEHIND_CAMERA"):
        project_ground_point(pixel, ground_behind_camera)

    non_planar = copy.deepcopy(base)
    non_planar["geometryValidity"] = "non-planar"
    non_planar["ground"]["geometryValidity"] = "non-planar"
    with pytest.raises(ValueError, match="geometryValidity"):
        FrameObservation.parse(non_planar)

    non_psd = copy.deepcopy(base)
    covariance = np.zeros((9, 9))
    covariance[0, 0] = covariance[1, 1] = 1.0
    covariance[0, 1] = covariance[1, 0] = 2.0
    non_psd["calibration"]["parameterCovariance"] = covariance.reshape(-1).tolist()
    with pytest.raises(ValueError, match="positive semidefinite"):
        FrameObservation.parse(non_psd)

    outside_roi = copy.deepcopy(base)
    outside_roi["ground"]["roiVertices"] = [
        [-1.0, -1.0, 0.0], [1.0, -1.0, 0.0],
        [1.0, 1.0, 0.0], [-1.0, 1.0, 0.0],
    ]
    parsed_outside_roi = FrameObservation.parse(outside_roi)
    outside_pixel = (float(base["calibration"]["cx"] + base["calibration"]["fx"]),
                     float(base["calibration"]["cy"]))
    with pytest.raises(ValueError, match="GROUND_INTERSECTION_OUTSIDE_ROI"):
        project_ground_point(outside_pixel, parsed_outside_roi.raw)

    non_finite = copy.deepcopy(base)
    non_finite["positionW"][0] = float("inf")
    with pytest.raises(ValueError, match="finite"):
        FrameObservation.parse(non_finite)

    uncertain_time = copy.deepcopy(base)
    uncertain_time["timeUncertaintyUs"] = 1
    uncertain_time["poseCovariance"] = None
    with pytest.raises(ValueError, match="TIME_UNCERTAINTY_UNBOUNDED"):
        project_ground_point(pixel, uncertain_time)


def test_demo_profile_covariance_matches_declared_virtual_error_model() -> None:
    profile = load_demo_profile(ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
    assert profile["acquisitionKind"] == "simulated"
    assert profile["accuracyQualification"] == "unvalidated"
    assert profile["anchorSigmaPixels"] == 2.0
    assert profile["clock"]["uncertaintyUs"] == 0
    angle_variance = math.radians(0.2) ** 2
    for camera in profile["cameras"].values():
        pose = np.asarray(camera["poseCovariance"], dtype=np.float64)
        assert np.diag(pose) == pytest.approx(
            [0.25 ** 2, 0.25 ** 2, 0.25 ** 2,
             angle_variance, angle_variance, angle_variance])
        assert np.count_nonzero(pose - np.diag(np.diag(pose))) == 0
        calibration = camera["calibration"]
        calibration_covariance = np.asarray(
            calibration["parameterCovariance"], dtype=np.float64)
        assert np.diag(calibration_covariance) == pytest.approx(
            [(0.01 * calibration["fx"]) ** 2,
             (0.01 * calibration["fy"]) ** 2, 1.0, 1.0, 0, 0, 0, 0, 0])
        assert np.count_nonzero(
            calibration_covariance - np.diag(np.diag(calibration_covariance))) == 0
    ground_covariance = np.asarray(profile["ground"]["parameterCovariance"], dtype=np.float64)
    assert np.diag(ground_covariance) == pytest.approx([0.0, 0.0, 0.0, 0.2 ** 2])
    assert np.count_nonzero(ground_covariance - np.diag(np.diag(ground_covariance))) == 0


@pytest.mark.parametrize("image_transform", ["raw", "rectified"])
def test_po04_distortion_and_rectified_image_projection(image_transform: str) -> None:
    camera = (0.0, 0.0, 100.0)
    yaw = (math.cos(math.radians(12.0)), 0.0, 0.0, math.sin(math.radians(12.0)))
    q_wc = _quaternion_multiply(yaw, (0.0, 1.0, 0.0, 0.0))
    target = (30.0, 15.0, 0.0)
    raw = _wide_analytic_observation("UAV1", 1, 1_000_000, camera, q_wc)
    raw["calibration"]["imageTransform"] = image_transform
    raw["calibration"]["distortion"] = [0.08, -0.01, 0.002, -0.001, 0.001]
    raw = FrameObservation.parse(raw).raw
    inverse = (q_wc[0], -q_wc[1], -q_wc[2], -q_wc[3])
    camera_ray = _rotate_quaternion(
        inverse, tuple(target[index] - camera[index] for index in range(3)))
    x, y = camera_ray[0] / camera_ray[2], camera_ray[1] / camera_ray[2]
    if image_transform == "raw":
        k1, k2, p1, p2, k3 = raw["calibration"]["distortion"]
        radius2 = x * x + y * y
        radial = 1.0 + k1 * radius2 + k2 * radius2 ** 2 + k3 * radius2 ** 3
        x_distorted = x * radial + 2.0 * p1 * x * y + p2 * (radius2 + 2.0 * x * x)
        y_distorted = y * radial + p1 * (radius2 + 2.0 * y * y) + 2.0 * p2 * x * y
    else:
        x_distorted, y_distorted = x, y
    calibration = raw["calibration"]
    pixel = (calibration["fx"] * x_distorted + calibration["cx"],
             calibration["fy"] * y_distorted + calibration["cy"])
    estimate = project_ground_point(pixel, raw)
    assert estimate.position_w == pytest.approx(target, abs=0.02)
    assert estimate.horizontal_range_m == pytest.approx(math.hypot(30.0, 15.0), abs=0.02)
    assert estimate.slant_range_m == pytest.approx(math.dist(target, camera), abs=0.02)


def test_po04_projection_intersects_a_tilted_ground_plane() -> None:
    camera = (0.0, 0.0, 100.0)
    target = (30.0, 15.0, 3.0)
    normal_raw = (0.1, -0.05, math.sqrt(1.0 - 0.1 ** 2 - 0.05 ** 2))
    normal_length = math.sqrt(sum(value * value for value in normal_raw))
    normal = tuple(value / normal_length for value in normal_raw)
    offset = -sum(normal[index] * target[index] for index in range(3))
    basis_u_raw = (1.0, 0.0, -normal[0] / normal[2])
    basis_u_length = math.sqrt(sum(value * value for value in basis_u_raw))
    basis_u = tuple(value / basis_u_length for value in basis_u_raw)
    basis_v_raw = (normal[1] * basis_u[2] - normal[2] * basis_u[1],
                   normal[2] * basis_u[0] - normal[0] * basis_u[2],
                   normal[0] * basis_u[1] - normal[1] * basis_u[0])
    basis_v_length = math.sqrt(sum(value * value for value in basis_v_raw))
    basis_v = tuple(value / basis_v_length for value in basis_v_raw)
    roi = []
    for sign_u, sign_v in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        roi.append([target[index] + 200.0 * sign_u * basis_u[index] +
                    200.0 * sign_v * basis_v[index] for index in range(3)])

    q_wc = _quaternion_multiply(
        (math.cos(math.radians(8.0)), 0.0, 0.0, math.sin(math.radians(8.0))),
        (0.0, 1.0, 0.0, 0.0))
    raw = _wide_analytic_observation("UAV1", 1, 1_000_000, camera, q_wc)
    raw["ground"].update({"normal": list(normal), "offsetM": offset,
                          "roiVertices": roi})
    raw = FrameObservation.parse(raw).raw
    pixel = _forward_pixel(target, camera, q_wc, focal_length=1000.0,
                           principal_point=(1000.0, 500.0))
    estimate = project_ground_point(pixel, raw)
    assert estimate.position_w == pytest.approx(target, abs=0.02)
    assert estimate.horizontal_range_m == pytest.approx(math.hypot(30.0, 15.0), abs=0.02)
    assert estimate.slant_range_m == pytest.approx(math.dist(target, camera), abs=0.02)
