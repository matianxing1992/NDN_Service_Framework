"""Analytic Spec210 geometry and source observation tests."""

from __future__ import annotations

import copy
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


def _write_verified_observation_bundle(tmp_path: Path, payload: bytes, *, sequence: int = 1,
                                       capture_time_us: int = 0,
                                       producers: dict[str, str] | None = None) -> tuple[list[dict], Path]:
    image_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    entries = []
    for camera in ("UAV1", "UAV2", "UAV3"):
        producer = producers[camera] if producers else f"/example/uav/drone/{camera}"
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


def test_verified_bundle_preserves_native_actual_producers(tmp_path: Path) -> None:
    producers = {"UAV1": "/lab/scout-a", "UAV2": "/lab/scout-b", "UAV3": "/lab/scout-c"}
    entries, path = _write_verified_observation_bundle(tmp_path, b"image-bytes", producers=producers)
    assert set(load_verified_observation_bundle(tmp_path, path)) == set(producers)
    entries[0]["producerIdentity"] = "/another/producer"
    path.write_text(json.dumps({
        "schema": "uav-verified-inputs-v2", "sourceEpoch": "7", "entries": entries,
    }), encoding="utf-8")
    with pytest.raises(ValueError, match="source producer namespace"):
        load_verified_observation_bundle(tmp_path, path)


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


@pytest.mark.parametrize("bound", [False, True])
def test_verified_observation_bundle_allows_external_calibration_provider(
        tmp_path: Path, bound: bool) -> None:
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    entries, manifest_path = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entry = entries[1]
    producer = "/example/calibration-provider"
    exact_name = f"{producer}/UAV/CALIBRATION/UAV2/1/v=99"
    sidecar_path = tmp_path / "UAV2.observation.json"
    sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
    if bound:
        exact_name = (f"{producer}/UAV/CALIBRATION/UAV2/1/" +
                      sidecar["calibrationRef"]["contentSha256"][7:] + "/v=99")
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


@pytest.mark.parametrize("bound", [False, True])
def test_verified_static_names_bind_digest_and_preserve_legacy(tmp_path: Path, bound: bool) -> None:
    entries, manifest_path = _write_verified_observation_bundle(tmp_path, b"image-bytes")
    manifest = json.loads(manifest_path.read_text())
    for entry in manifest["entries"]:
        sidecar_path = tmp_path / (entry["cameraId"] + ".observation.json")
        sidecar = json.loads(sidecar_path.read_text())
        for ref in entry["references"]:
            category = ref["category"]
            if category not in {"CLOCK", "CALIBRATION", "GROUND"}:
                continue
            key = {"CLOCK": "clockRef", "CALIBRATION": "calibrationRef", "GROUND": "groundRef"}[category]
            if bound:
                prefix, version = ref["exactName"].rsplit("/", 1)
                ref["exactName"] = prefix + "/" + ref["contentSha256"][7:] + "/" + version
            sidecar[key]["exactName"] = ref["exactName"]
        encoded = json.dumps(sidecar, sort_keys=True, separators=(",", ":")).encode()
        sidecar_path.write_bytes(encoded)
        entry["observationDigest"] = "sha256:" + hashlib.sha256(encoded).hexdigest()
    manifest_path.write_text(json.dumps(manifest))
    assert set(load_verified_observation_bundle(tmp_path, manifest_path)) == {"UAV1", "UAV2", "UAV3"}
    if bound:
        entry = manifest["entries"][0]
        ref = next(ref for ref in entry["references"] if ref["category"] == "CLOCK")
        prefix, digest, version = ref["exactName"].rsplit("/", 2)
        wrong = "b" * 64 if digest != "b" * 64 else "a" * 64
        ref["exactName"] = prefix + "/" + wrong + "/" + version
        sidecar_path = tmp_path / (entry["cameraId"] + ".observation.json")
        sidecar = json.loads(sidecar_path.read_text())
        sidecar["clockRef"]["exactName"] = ref["exactName"]
        encoded = json.dumps(sidecar, sort_keys=True, separators=(",", ":")).encode()
        sidecar_path.write_bytes(encoded)
        entry["observationDigest"] = "sha256:" + hashlib.sha256(encoded).hexdigest()
        manifest_path.write_text(json.dumps(manifest))
        with pytest.raises(ValueError, match="invalid typed structure"):
            load_verified_observation_bundle(tmp_path, manifest_path)
