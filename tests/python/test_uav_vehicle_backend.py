"""Model adapter boundaries; these tests do not qualify detector or native tracking accuracy."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from PIL import Image
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
import tracking_worker as worker  # noqa: E402


class ControlledDetector:
    instances = []

    def __init__(self, model, **options):
        self._models = {}
        self._byte_track_state = {}
        self.closed = False
        self.cameras = []
        self.instances.append(self)

    def start(self, profile, cameras):
        assert profile["device"] == "cpu"

    def _detect(self, camera, image):
        self.cameras.append(camera)
        return [((10, 20, 50, 60), 0.9, "traffic light", None)]

    def process_window(self, *args, **kwargs):
        raise AssertionError("backend must not decide application IDs or motion")

    def _assign_ids(self, *args, **kwargs):
        raise AssertionError("backend must not decide application IDs")

    def close(self):
        self.closed = True


@pytest.fixture
def inputs(tmp_path):
    ControlledDetector.instances.clear()
    frames = []
    for camera in worker.CAMERAS:
        path = tmp_path / (camera + ".jpg")
        Image.new("RGB", (100, 100)).save(path, format="JPEG")
        frames.append({"cameraId": camera, "inputDigest": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
                       "width": 100, "height": 100})
    window = {"schema": "uav-vehicle-backend-input-v1", "runId": "vehicle-run",
              "missionId": "tracking-vehicle-run", "windowId": "window-0", "sequence": 1,
              "nativeDecisionDigest": "sha256:" + "a" * 64, "frames": frames,
              "previousBackendState": {"schema": "uav-vehicle-backend-state-v1",
                                       "cameras": {camera: None for camera in worker.CAMERAS}}}
    path = tmp_path / "vehicle-window.json"
    path.write_text(json.dumps(window))
    arguments = argparse.Namespace(vehicle_window=str(path), input_dir=str(tmp_path),
        output_json=str(tmp_path / "candidates.json"), model=str(tmp_path / "model"),
        run_id=window["runId"], mission_id=window["missionId"], window_id=window["windowId"],
        sequence=1, tracking_profile=None, state_file=None, candidate_state_file=None,
        motion_json=None, observation_dir=None, observation_manifest=None, confidence=0.25)
    return arguments, window, path


def test_candidates_have_no_final_ids_or_motion_and_close_the_model(inputs):
    arguments, _, _ = inputs
    assert worker._run_vehicle_backend(arguments, ControlledDetector) == 0
    result = json.loads(Path(arguments.output_json).read_text())
    assert result["schema"] == "uav-vehicle-backend-window-v1"
    assert result["actualDevice"] == "cpu"
    assert [frame["cameraId"] for frame in result["frames"]] == list(worker.CAMERAS)
    for frame in result["frames"]:
        detection, = frame["detections"]
        assert set(detection) == {"box", "confidence", "class", "byteTrackId"}
        assert detection["class"] == "traffic light"
        assert detection["byteTrackId"] is None
        assert "tracks" not in frame and "measurements" not in frame
    assert ControlledDetector.instances[0].closed
    assert ControlledDetector.instances[0].cameras == list(worker.CAMERAS)


@pytest.mark.parametrize("fault", ["digest", "dimensions", "last-jpeg", "binding", "duplicate"])
def test_bad_bundle_is_rejected_before_loading_or_mutating_the_model(inputs, fault):
    arguments, window, path = inputs
    if fault == "digest":
        window["frames"][0]["inputDigest"] = "sha256:" + "f" * 64
    elif fault == "dimensions":
        window["frames"][0]["width"] = 200
    elif fault == "last-jpeg":
        payload = b"invalid-jpeg"
        (Path(arguments.input_dir) / "UAV3.jpg").write_bytes(payload)
        window["frames"][2]["inputDigest"] = "sha256:" + hashlib.sha256(payload).hexdigest()
    elif fault == "binding":
        window["missionId"] = "other-mission"
    path.write_text(json.dumps(window))
    if fault == "duplicate":
        path.write_text(path.read_text().replace('"sequence": 1', '"sequence": 1, "sequence": 1'))
    with pytest.raises((ValueError, OSError)):
        worker._run_vehicle_backend(arguments, ControlledDetector)
    assert ControlledDetector.instances == []
    assert not Path(arguments.output_json).exists()


def test_backend_failure_closes_resources_and_never_writes_application_state(inputs):
    arguments, _, _ = inputs

    class FailingDetector(ControlledDetector):
        def _detect(self, camera, image):
            raise RuntimeError("controlled detector failure")

    with pytest.raises(RuntimeError, match="controlled detector failure"):
        worker._run_vehicle_backend(arguments, FailingDetector)
    assert FailingDetector.instances[-1].closed
    assert not Path(arguments.output_json).exists()
    assert not (Path(arguments.input_dir) / "tracking-state.json").exists()


def test_detection_limit_is_not_silently_truncated(inputs):
    arguments, _, _ = inputs

    class ExcessDetector(ControlledDetector):
        def _detect(self, camera, image):
            return super()._detect(camera, image) * 301

    with pytest.raises(ValueError, match="detection limit"):
        worker._run_vehicle_backend(arguments, ExcessDetector)
    assert ExcessDetector.instances[-1].closed
    assert not Path(arguments.output_json).exists()


def test_native_numerical_export_matches_independent_python_oracle():
    """Optional batch oracle consumes values produced by the actual native test executable."""
    import os
    from motion_estimator import MotionEstimator, project_ground_point

    path = os.environ.get("NDNSF_UAV_VEHICLE_ORACLE_OUTPUT")
    if not path:
        pytest.skip("native numerical export must be produced by the B1b C++ selector")
    cases = json.loads(Path(path).read_text(encoding="utf-8"))
    assert [case["case"] for case in cases] == [
        "moving", "moving-camera", "stationary", "missing-covariance",
        "nonzero-covariance", "distortion"]
    comparisons = 0
    for case in cases:
        estimator = MotionEstimator()
        assert len(case["samples"]) == 3
        for sample in case["samples"]:
            raw, native = sample["raw"], sample["native"]
            identity = raw["identity"]
            key = (identity["sourceId"], identity["cameraId"], identity["epoch"], 1, 1)
            ground = project_ground_point(tuple(sample["anchor"]), raw)
            expected = estimator.update(key, raw["captureTimeUs"], ground)
            assert native["speedStatus"] == expected["speedStatus"]
            assert native["bearingStatus"] == expected["bearingStatus"]
            assert native["quality"] == expected["quality"]
            assert native["reasonCodes"] == ([] if expected["reason"] == "OK"
                                              else [expected["reason"]])
            assert native["estimateTimeUs"] == expected["estimateTimeUs"]
            pairs = {
                "speedMps": expected["speedMps"],
                "courseDeg": expected["bearingDegreesTrue"],
                "horizontalRangeM": ground.horizontal_range_m,
                "slantRangeM": ground.slant_range_m,
                "sigmaRangeM": ground.range_sigma_m,
                "sigmaSpeedMps": expected.get("sigmaSpeedMps"),
                "sigmaCourseDeg": expected.get("sigmaBearingDegrees"),
                "velocityEastMps": expected.get("velocityENU_mps", [None, None])[0],
                "velocityNorthMps": expected.get("velocityENU_mps", [None, None])[1],
            }
            for field, value in pairs.items():
                if value is None:
                    assert native[field] is None, (case["case"], field)
                else:
                    assert native[field] == pytest.approx(value, rel=1e-8, abs=1e-9), (
                        case["case"], field)
                comparisons += 1
    assert comparisons == 162
