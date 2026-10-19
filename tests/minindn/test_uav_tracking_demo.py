"""Preflight and explicit opt-in MiniNDN qualification of the five-node harness.

Preflight checks run without a network experiment. Opt-in qualification uses
installed native programs, real video/model inputs and an explicit root check.
"""

from __future__ import annotations

import os
import csv
import hashlib
import json
import math
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import textwrap
from contextlib import contextmanager

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from Experiments.UAV.run_multicamera_tracking_demo import (  # noqa: E402
    DEFAULT_OBSERVATION_PROFILE, build_plan, preflight)
from Experiments.UAV.tracking_topology import EXPECTED_NODES, Topology, TopologyError  # noqa: E402

ASSETS = Path(os.environ.get(
    "NDNSF_UAV_DEMO_ASSET_DIR", "/home/tianxing/.cache/ndnsf/spec191-assets"
)).expanduser()

MODEL = Path(os.environ.get("NDNSF_UAV_DEMO_MODEL", str(ASSETS / "3UAVs.pt"))).expanduser()

CONTROLLED_CAMERA_VELOCITIES = {
    "UAV1": [5.0, 0.0, 0.0],
    "UAV2": [0.0, -5.0, 0.0],
    "UAV3": [-3.0, 4.0, 0.0],
}


def _write_controlled_metric_profile(directory: Path) -> Path:
    source_bytes = DEFAULT_OBSERVATION_PROFILE.read_bytes()
    profile = json.loads(source_bytes)
    for camera, velocity in CONTROLLED_CAMERA_VELOCITIES.items():
        profile["cameras"][camera]["velocityWmps"] = velocity
    profile["sourceIdPrefix"] = "uav-controlled-qualification"
    output = directory / "controlled-observation-profile.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output_bytes = (json.dumps(profile, indent=2, sort_keys=True) + "\n").encode()
    if output.exists():
        assert output.read_bytes() == output_bytes, output
    else:
        output.write_bytes(output_bytes)
    assert hashlib.sha256(DEFAULT_OBSERVATION_PROFILE.read_bytes()).digest() == \
        hashlib.sha256(source_bytes).digest()
    return output


@contextmanager
def _metric_output_directory(prefix: str, mode: str):
    retained_root = os.environ.get("NDNSF_UAV_METRIC_OUTPUT_ROOT")
    if retained_root:
        root = Path(retained_root).expanduser().resolve()
        root.mkdir(parents=True, exist_ok=True)
        output = root / f"{prefix}{mode}"
        if output.exists():
            raise AssertionError(f"metric run output already exists: {output}")
        yield output
        return
    with tempfile.TemporaryDirectory() as directory:
        yield Path(directory) / f"{prefix}{mode}"


def _metric_profile_directory(tmp_path: Path) -> Path:
    retained_root = os.environ.get("NDNSF_UAV_METRIC_OUTPUT_ROOT")
    return Path(retained_root).expanduser().resolve() / "profiles" if retained_root else tmp_path


def _run_metric_profile(mode, *, require_course_every_camera, profile_path: Path):
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    desktop = mode == "live-push" and os.environ.get("NDNSF_UAV_DEMO_DESKTOP") == "1"
    prefix = "metric-100-" if require_course_every_camera else "hover-100-"
    with _metric_output_directory(prefix, mode) as output:
        command = [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
                   "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
                   "--license", "Apache-2.0", "--window-count", "100", "--observation-mode", mode,
                   "--observation-profile", str(profile_path), "--run", "--run-seconds", "3600",
                   "--global-deadline-ms", "120000"]
        if not desktop:
            command.append("--headless")
        result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=3900)
        assert result.returncode == 0, result.stdout + "\n" + result.stderr
        worker = output / "private/compute/worker"
        observation_plan = json.loads((output / "manifest.json").read_text())["observation"]
        for path_key, digest_key in (("rawProfileSnapshot", "profileDigest"),
                                     ("effectiveProfileSnapshot", "effectiveProfileDigest")):
            assert "sha256:" + hashlib.sha256(Path(observation_plan[path_key]).read_bytes()).hexdigest() == \
                observation_plan[digest_key]
        results = sorted((worker / "results").glob("window-*.json"))
        assert len(results) == 100
        frames = {camera: set() for camera in ("UAV1", "UAV2", "UAV3")}
        qualified = {camera: set() for camera in frames}
        expected_display = {}
        for path in results:
            payload = json.loads(path.read_text())
            sequence = int(path.stem.split("-")[-1]) + 1
            measurements = payload["motion"]["measurements"]
            frame_bindings = {}
            # Capture coverage includes frames with no detected vehicles.
            assert len(payload["frames"]) == len(frames), path
            for frame in payload["frames"]:
                camera = frame["cameraId"]
                assert camera in frames and camera not in frame_bindings, path
                assert frame["sequence"] == sequence, (path, camera)
                frame_bindings[camera] = tuple(frame[field] for field in (
                    "sourceId", "cameraId", "sourceEpoch", "sequence"))
                frames[camera].add(sequence)
                summary = frame["observation"]
                assert summary["identity"]["cameraId"] == camera
                assert summary["identity"]["sourceId"] == frame["sourceId"]
                assert int(summary["identity"]["epoch"]) == frame["sourceEpoch"]
                assert summary["anchorApproximation"] is True
                assert summary["timeUncertaintyUs"] is not None
                assert float(summary["clockBinding"]["a"]) > 0
                expected_display[camera, sequence] = summary

            def identity(measurement):
                frame, track = measurement["frameKey"], measurement["trackKey"]
                return tuple(str(value) for value in (
                    frame["sourceId"], frame["cameraId"], frame["sourceEpoch"],
                    frame["sequence"], track["localTrackId"], track["trackGeneration"]))

            expected = {identity(item): item for item in measurements}
            assert len(expected) == len(measurements), path
            csv_path = worker / "annotated" / f"motion-measurements-{sequence:06d}.csv"
            with csv_path.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            fields = ("sourceId", "cameraId", "sourceEpoch", "sequence", "localTrackId", "trackGeneration")
            actual = {tuple(row[field] for field in fields): row for row in rows}
            assert len(actual) == len(rows) and actual.keys() == expected.keys(), csv_path
            for key, item in expected.items():
                row = actual[key]
                for field in ("speedMps", "courseDeg", "horizontalRangeM", "slantRangeM",
                              "velocityEastMps", "velocityNorthMps"):
                    if item[field] is None:
                        assert row[field] == "", (csv_path, key, field)
                    else:
                        assert float(row[field]) == pytest.approx(item[field], abs=1e-9)
                for field in ("captureTimeUs", "estimateTimeUs", "rangeTimeUs"):
                    assert row[field] == ("" if item[field] is None else str(item[field]))
                for field in ("speedStatus", "bearingStatus", "rangeStatus", "quality",
                              "acquisitionKind", "geometryValidity", "accuracyQualification"):
                    assert row[field] == item[field]
                assert item["acquisitionKind"] == "simulated"
                assert item["geometryValidity"] == "illustrative"
                assert item["accuracyQualification"] == "unvalidated"
                assert item["anchorApproximation"] is True
                assert row["anchorApproximation"] == "true"
                for status, fields_with_values in {
                    "speedStatus": ("speedMps", "velocityEastMps", "velocityNorthMps"),
                    "bearingStatus": ("courseDeg",),
                    "rangeStatus": ("horizontalRangeM", "slantRangeM"),
                }.items():
                    if item[status] == "valid":
                        for field in fields_with_values:
                            value = item[field]
                            assert isinstance(value, (int, float)) and not isinstance(value, bool)
                            assert math.isfinite(value), (path, key, field)
                if item["speedStatus"] == "valid":
                    assert item["speedMps"] >= 0
                if item["bearingStatus"] == "valid":
                    assert 0 <= item["courseDeg"] < 360
                else:
                    assert item["courseDeg"] is None
                    assert item["imageArrow"] is None
                if item["rangeStatus"] == "valid":
                    assert item["slantRangeM"] >= 0
                    assert 0 <= item["horizontalRangeM"] <= item["slantRangeM"] + 1e-9
                camera = item["frameKey"]["cameraId"]
                frame_sequence = item["frameKey"]["sequence"]
                assert tuple(item["frameKey"][field] for field in (
                    "sourceId", "cameraId", "sourceEpoch", "sequence")) == frame_bindings[camera]
                track = json.dumps(item["trackKey"], sort_keys=True)
                required_statuses = (("speedStatus", "bearingStatus", "rangeStatus")
                                     if require_course_every_camera else ("speedStatus", "rangeStatus"))
                if all(item[field] == "valid" for field in required_statuses):
                    qualified[camera].add((track, frame_sequence))
        for camera in frames:
            assert frames[camera] == set(range(1, 101)), camera
            assert any((track, sequence - 1) in qualified[camera] and
                       (track, sequence - 2) in qualified[camera]
                       for track, sequence in qualified[camera]), (
                           camera, "no continuous mature track for required statuses", require_course_every_camera)
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True and lifecycle["outcome"] == "collaboration-result"
        display = (output / "compute-display.log").read_text(errors="replace")
        shown = []
        for line in display.splitlines():
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("event") == "DISPLAY_FRAME_SHOWN":
                shown.append(event)
        assert len(shown) == 300
        assert {(event["cameraId"], event["sequence"]) for event in shown} == {
            (camera, sequence) for camera in frames for sequence in range(1, 101)}
        delivery = "live-push" if mode == "live-push" else "replay"
        assert all(event["accuracyQualification"] == "unvalidated" and
                   event["provenance"] == f"simulated/{delivery}" for event in shown)
        clock_basis = "simulation" if mode == "live-push" else "playback"
        for event in shown:
            summary = expected_display[event["cameraId"], event["sequence"]]
            assert event["sourceIdentity"] == summary["identity"]
            assert event["observationRef"] == summary["observationRef"]
            assert event["geometryValidity"] == "illustrative"
            assert event["anchorApproximation"] is True
            assert event["speedDisplayUnit"] == "km/h"
            age = event["captureAge"]
            assert age["basis"] in {clock_basis, "unknown"}
            if age["age_ms"] is not None:
                assert age["basis"] == clock_basis
                assert math.isfinite(age["age_ms"]) and age["age_ms"] >= 0
                assert age["uncertainty_ms"] >= 0
                expected_freshness = ("fresh" if age["age_ms"] + age["uncertainty_ms"] <= 1000
                                      else "stale" if age["age_ms"] - age["uncertainty_ms"] > 1000
                                      else "unknown")
                assert age["freshness"] == expected_freshness
            else:
                assert age["freshness"] == "unknown"
        for camera in frames:
            assert any(event["cameraId"] == camera and
                       event["captureAge"]["basis"] == clock_basis and
                       event["captureAge"]["age_ms"] is not None for event in shown), camera


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_METRIC_DEMO") != "1",
                    reason="set NDNSF_RUN_MININDN_METRIC_DEMO=1 for all-camera course acceptance")
@pytest.mark.parametrize("mode", ["simulated", "replay", "live-push"])
def test_minindn_metric_demo_has_mature_course_in_every_camera(mode, tmp_path):
    profile = _write_controlled_metric_profile(_metric_profile_directory(tmp_path))
    _run_metric_profile(mode, require_course_every_camera=True, profile_path=profile)


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_HOVER_DEMO") != "1",
                    reason="set NDNSF_RUN_MININDN_HOVER_DEMO=1 for hover quality/export acceptance")
@pytest.mark.parametrize("mode", ["simulated", "replay", "live-push"])
def test_minindn_hover_demo_preserves_metric_quality_for_every_camera(mode):
    _run_metric_profile(mode, require_course_every_camera=False,
                        profile_path=DEFAULT_OBSERVATION_PROFILE)


def test_controlled_metric_profile_preserves_default_hover_profile(tmp_path):
    original_digest = hashlib.sha256(DEFAULT_OBSERVATION_PROFILE.read_bytes()).digest()
    profile_path = _write_controlled_metric_profile(tmp_path)
    profile = json.loads(profile_path.read_text())
    assert {camera: data["velocityWmps"] for camera, data in profile["cameras"].items()} == \
        CONTROLLED_CAMERA_VELOCITIES
    default_profile = json.loads(DEFAULT_OBSERVATION_PROFILE.read_text())
    assert all(data["velocityWmps"] == [0.0, 0.0, 0.0]
               for data in default_profile["cameras"].values())
    assert hashlib.sha256(DEFAULT_OBSERVATION_PROFILE.read_bytes()).digest() == original_digest



@pytest.mark.skipif(not (ASSETS / "uav1_1min.mp4").is_file(), reason="Spec191 assets unavailable")
def test_preflight_keeps_controller_inside_ground_station_node():
    with tempfile.TemporaryDirectory() as directory:
        plan = build_plan(source=ASSETS, model=MODEL,
                          output=Path(directory), license_text="local-test-accepted",
                          headless=True)
        report = preflight(plan, run=False, headless=True)
    assert report["ok"]
    assert set(report["nodes"]) == EXPECTED_NODES
    assert report["processCount"] == 7  # five network nodes + gs-local Controller + renderer


@pytest.mark.skipif(not (ASSETS / "uav1_1min.mp4").is_file(), reason="Spec191 assets unavailable")
def test_process_plan_uses_real_tracking_ground_station_and_scoped_policy():
    with tempfile.TemporaryDirectory() as directory:
        plan = build_plan(source=ASSETS, model=MODEL,
                          output=Path(directory), license_text="local-test-accepted",
                          headless=True)
    by_name = {item["name"]: item for item in plan["processes"]}
    assert "UavTrackingGroundStation" in by_name["ground-station"]["command"][0]
    controller = " ".join(by_name["controller"]["command"])
    assert "Experiments/UAV/spec191.policies" in controller
    assert by_name["controller"]["node"] == by_name["ground-station"]["node"] == "gs"


@pytest.mark.skipif(not (ASSETS / "uav1_1min.mp4").is_file(), reason="Spec191 assets unavailable")
def test_motion_fixture_is_bound_to_source_compute_and_ground_station():
    with tempfile.TemporaryDirectory() as directory:
        plan = build_plan(source=ASSETS, model=MODEL,
                          output=Path(directory), license_text="local-test-accepted",
                          headless=True, window_count=3, motion_profile="nominal")
        motion = plan["motion"]
        fixture = Path(motion["directory"])
        assert motion["schema"] == "spec191-motion-fixture-v1"
        assert motion["provenance"] == "simulated-input"
        assert (fixture / "window-0.json").is_file()
        assert (fixture / "window-2.json").is_file()
    for name in ("ground-station", "uav1-camera", "uav2-camera", "uav3-camera", "compute"):
        command = by_name = next(item["command"] for item in plan["processes"]
                                if item["name"] == name)
        assert "--motion-dir" in command


def test_extra_network_controller_is_rejected():
    with pytest.raises(TopologyError):
        Topology(frozenset((*EXPECTED_NODES, "controller")), frozenset()).validate()


@pytest.mark.skipif(not (ASSETS / "uav1_1min.mp4").is_file(), reason="Spec191 assets unavailable")
def test_run_preflight_reports_environment_blockers_without_starting_network():
    with tempfile.TemporaryDirectory() as directory:
        plan = build_plan(source=ASSETS, model=MODEL,
                          output=Path(directory), license_text="local-test-accepted",
                          headless=True)
        report = preflight(plan, run=True, headless=True)
    if report["ok"]:
        pytest.skip("native binaries and MiniNDN root privilege are available")
    assert any("native binary" in item or "root" in item for item in report["errors"])


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_SOURCE_SWITCH") != "1",
                    reason="set NDNSF_RUN_MININDN_SOURCE_SWITCH=1 for the bounded MiniNDN acceptance")
def test_same_minindn_run_switches_source_generation_and_commits_only_matching_results():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "source-switch"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0",
             "--window-count", "4", "--source-switch-after-window", "2",
             "--headless", "--run", "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=300)
        assert result.returncode == 0, result.stdout + "\n" + result.stderr
        manifest = json.loads((output / "manifest.json").read_text())
        generations = manifest["observation"]["sourceGenerations"]["generations"]
        assert [item["firstWindowIndex"] for item in generations] == [0, 2]
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_SOURCE_GENERATION_ACTIVATED first_window=2" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=2 status=success" in ground_station
        assert "SPEC191_TRACKING_RESULT_CONTEXT_VERIFY_FAILED" not in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-result"
        results = output / "private/compute/worker/results"
        for index in range(4):
            result_json = json.loads((results / f"window-{index}.json").read_text())
            generation = generations[0] if index < 2 else generations[1]
            assert result_json["terminal"] is True
            assert [frame["sourceId"] for frame in result_json["frames"]] == [
                generation["sourceIds"][camera] for camera in ("UAV1", "UAV2", "UAV3")]
            assert {frame["sourceEpoch"] for frame in result_json["frames"]} == {
                generation["sourceEpoch"]}


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_SOURCE_SWITCH") != "1",
                    reason="set NDNSF_RUN_MININDN_SOURCE_SWITCH=1 for the bounded MiniNDN acceptance")
def test_minindn_rejects_old_generation_response_with_a_valid_digest():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "stale-generation"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0",
             "--window-count", "4", "--source-switch-after-window", "2",
             "--fault", "stale-generation-result", "--headless", "--run",
            "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=300)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_STALE_GENERATION_RESPONSE_INJECTED window=window-2" in compute
        assert "SPEC191_TRACKING_RESULT_CONTEXT_VERIFY_FAILED window=2" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=2 status=failure" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=2 status=success" not in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-failed"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_DEPENDENCY_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_DEPENDENCY_FAULTS=1 for the MiniNDN dependency-fault acceptance")
@pytest.mark.parametrize("category,kind,expected_reason", [
    (category, kind, reason)
    for category in ("CLOCK", "CALIBRATION", "GROUND", "POSE")
    for kind, reason in (
        ("content", "observation dependency content digest mismatch"),
        ("producer", "DataRef name is not canonical or producer-owned"),
        (("epoch", "DataRef has an invalid typed name shape") if category == "POSE" else
         ("revision", "exact name does not match its record revision")),
    )
])
def test_minindn_rejects_replaced_observation_dependency_before_worker(category, kind, expected_reason):
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    fault = f"bad-reference-{kind}-{category.lower()}"
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / fault
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "2", "--fault", fault,
             "--observation-mode", "live-push", "--headless", "--run", "--run-seconds", "180"],
            check=False, capture_output=True, text=True, timeout=300)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        source = (output / "uav3-camera.log").read_text(errors="replace")
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert (f"SPEC210_REFERENCE_OBJECT_INJECTED camera=UAV3 window=window-1 "
                f"kind={kind} category={category}") in source
        assert expected_reason in compute
        if kind == "revision":
            assert (f"SPEC210_REFERENCE_DIGEST_VERIFIED camera=UAV3 window=window-1 "
                    f"category={category} exactName=") in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-0" in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-1" not in compute
        assert "SPEC191_TRACKING_RESULT status=failure window=1" in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-failed"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_DEPENDENCY_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_DEPENDENCY_FAULTS=1 for the MiniNDN dependency-fault acceptance")
@pytest.mark.parametrize("fault,category", [
    ("bad-reference-digest", "CLOCK"),
    ("bad-reference-digest-calibration", "CALIBRATION"),
    ("bad-reference-digest-ground", "GROUND"),
    ("bad-reference-digest-pose", "POSE"),
])
def test_minindn_rejects_bad_observation_reference_digest_terminally(fault, category):
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / fault
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0",
             "--window-count", "2", "--fault", fault,
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "180"],
            check=False, capture_output=True, text=True, timeout=300)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        source = (output / "uav3-camera.log").read_text(errors="replace")
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert ("SPEC191_TRACKING_BAD_REFERENCE_DIGEST_INJECTED camera=UAV3 "
                f"window=window-1 category={category}") in source
        assert "observation dependency content digest mismatch" in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-0" in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-1" not in compute
        assert "SPEC191_TRACKING_RESULT status=failure window=1" in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-failed"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_DEPENDENCY_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_DEPENDENCY_FAULTS=1 for the MiniNDN dependency-fault acceptance")
def test_minindn_missing_clock_reference_fails_before_worker_terminally():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "missing-reference-clock"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "2",
             "--fault", "missing-reference-clock", "--observation-mode", "live-push",
             "--headless", "--run", "--run-seconds", "180"],
            check=False, capture_output=True, text=True, timeout=300)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        source = (output / "uav3-camera.log").read_text(errors="replace")
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_REFERENCE_OMITTED camera=UAV3 window=window-1 category=CLOCK" in source
        assert "tracking compute could not fetch" in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-0" in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-1" not in compute
        assert "SPEC191_TRACKING_RESULT status=failure window=1" in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-failed"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_DEPENDENCY_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_DEPENDENCY_FAULTS=1 for the MiniNDN dependency-fault acceptance")
@pytest.mark.parametrize("fault,expected_reason", [
    ("duplicate-reference-pose", "observation closure contains a duplicate exact Data name"),
    ("overlimit-reference-pose", "observation pose status does not match its pose references or estimate"),
])
def test_minindn_rejects_duplicate_or_overlimit_pose_references_before_worker(fault, expected_reason):
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / fault
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "2", "--fault", fault,
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "180"],
            check=False, capture_output=True, text=True, timeout=300)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        source = (output / "uav3-camera.log").read_text(errors="replace")
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert (f"SPEC210_REFERENCE_SHAPE_INJECTED camera=UAV3 window=window-1 "
                f"shape={fault}") in source
        assert expected_reason in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-0" in compute
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-1" not in compute
        assert "SPEC191_TRACKING_RESULT status=failure window=1" in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-failed"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_cancel_after_selection_is_terminal():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "cancel-after-selection"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0",
             "--window-count", "3", "--fault", "cancel",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=120)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC191_TRACKING_CANCELLED window=0" in ground_station
        assert "SPEC210_CANCEL_ACK_CONFIRMED window=0" in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-cancelled"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_terminal_response_wins_user_cancel_race():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "terminal-response-cancel-race"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "1",
             "--fault", "user-cancel-terminal-response-race",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=240)
        assert result.returncode == 0, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_TERMINAL_RESPONSE_LOCKED window=window-0" in compute
        assert "SPEC210_TERMINAL_RESPONSE_PUBLISHED window=window-0" in compute
        assert "SPEC210_USER_CANCEL_WAITING_FOR_TERMINAL_LOCK window=window-0" in compute
        assert "SPEC210_USER_CANCEL_ACK_HELD window=window-0" in compute
        assert compute.index("SPEC210_TERMINAL_RESPONSE_PUBLISHED") < compute.index(
            "SPEC210_USER_CANCEL_ACK_HELD")
        assert "SPEC210_CANCEL_NOTICE_QUEUED window=0 accepted=true" in ground_station
        assert "SPEC210_TERMINAL_RESPONSE_CALLBACK_HELD window=0" in ground_station
        assert "SPEC210_CANCEL_RACE_WAITING_FOR_RESPONSE window=0" in ground_station
        assert "SPEC210_TERMINAL_RESPONSE_DEFERRED window=0" in ground_station
        assert "SPEC210_CANCEL_RACE_TERMINAL_RESPONSE_WON window=0" in ground_station
        assert "SPEC210_CANCEL_ACK_CONFIRMED window=0" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" in ground_station
        assert "SPEC191_TRACKING_CANCELLED window=0" not in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-result"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_user_cancel_wins_terminal_response_race():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "user-cancel-terminal-response-race"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "1",
             "--fault", "user-cancel-terminal-response-cancel-wins-race",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=240)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_TERMINAL_RESPONSE_READY window=window-0" in compute
        assert "SPEC210_USER_CANCEL_WAITING_FOR_TERMINAL_LOCK window=window-0" in compute
        assert "SPEC210_USER_CANCEL_ACK_HELD window=window-0" in compute
        assert "SPEC210_USER_CANCEL_ROLE_CLOSED" in compute
        assert compute.index("SPEC210_USER_CANCEL_ACK_HELD") < compute.index(
            "SPEC210_USER_CANCEL_ROLE_CLOSED")
        assert "SPEC210_TERMINAL_RESPONSE_PUBLISHED window=window-0" not in compute
        assert "SPEC210_CANCEL_NOTICE_QUEUED window=0 accepted=true" in ground_station
        assert "SPEC210_CANCEL_ACK_CONFIRMED window=0" in ground_station
        assert "SPEC191_TRACKING_CANCELLED window=0 accepted=true" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" not in ground_station
        assert "SPEC210_TERMINAL_RESPONSE_CALLBACK_HELD window=0" not in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-cancelled"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_revokes_authority_during_active_segmented_fetch():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "cancel-during-active-fetch"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "1",
             "--global-deadline-ms", "20000", "--fault", "cancel-during-active-fetch",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=180)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        source = (output / "uav3-camera.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_ACTIVE_FETCH_SOURCE_HELD camera=UAV3 window=window-0" in source
        assert "SPEC210_ACTIVE_FETCH_CANCEL_TRIGGERED camera=UAV3 window=window-0" in compute
        assert "SPEC210_ACTIVE_FETCH_CANCELLED camera=UAV3 window=window-0" in compute
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" not in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] in {"collaboration-failed", "collaboration-timeout"}


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_revokes_authority_during_active_reference_fetch():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "cancel-during-active-reference-fetch"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "1",
             "--global-deadline-ms", "20000", "--fault", "cancel-during-active-reference-fetch",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=180)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        source = (output / "uav3-camera.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_ACTIVE_REFERENCE_SOURCE_HELD camera=UAV3 window=window-0 category=POSE" in source
        assert "SPEC210_ACTIVE_REFERENCE_CANCEL_TRIGGERED camera=UAV3 window=window-0 category=POSE authority_revoked=true" in compute
        assert "SPEC210_ACTIVE_REFERENCE_CANCELLED camera=UAV3 window=window-0 category=POSE" in compute
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" not in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] in {"collaboration-failed", "collaboration-timeout"}


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_user_cancels_during_active_reference_fetch():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "user-cancel-during-active-reference-fetch"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "1",
             "--global-deadline-ms", "20000",
             "--fault", "user-cancel-during-active-reference-fetch",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=180)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        source = (output / "uav3-camera.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_ACTIVE_REFERENCE_SOURCE_HELD camera=UAV3 window=window-0 category=POSE" in source
        assert "SPEC210_ACTIVE_REFERENCE_FETCH_STARTED camera=UAV3 window=window-0 category=POSE" in compute
        assert "SPEC210_CANCEL_NOTICE_QUEUED window=0 accepted=true" in ground_station
        assert "SPEC210_USER_CANCEL_OBSERVED window=window-0" in compute
        assert "SPEC210_USER_CANCEL_ACK_QUEUED window=window-0 authority_revoked=true" in compute
        assert "SPEC210_CANCEL_ACK_CONFIRMED window=0" in ground_station
        assert "SPEC210_USER_CANCEL_FAILURE_SUPPRESSED" in compute
        assert "SPEC210_USER_CANCEL_LATE_WORKER_COMPLETED window=window-0" not in compute
        assert "SPEC191_TRACKING_CANCELLED window=0 accepted=true" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" not in ground_station
        assert not list((output / "private/compute/worker/transactions").glob(
            "*/window-0/manifest.json"))
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-cancelled"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_cancel_after_source_cutover_discards_candidate_state():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "cancel-after-source-cutover"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "2",
             "--source-switch-after-window", "1", "--global-deadline-ms", "20000",
             "--fault", "user-cancel-after-source-switch-during-active-reference-fetch",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=240)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        source = (output / "uav3-camera.log").read_text(errors="replace")
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" in ground_station
        assert "SPEC210_SOURCE_GENERATION_ACTIVATED first_window=1" in ground_station
        assert "SPEC210_ACTIVE_REFERENCE_FETCH_STARTED camera=UAV3 window=window-1 category=POSE" in compute
        assert "SPEC210_ACTIVE_REFERENCE_SOURCE_HELD camera=UAV3 window=window-1 category=POSE" in source
        assert "SPEC210_CANCEL_NOTICE_QUEUED window=1 accepted=true" in ground_station
        assert "SPEC210_USER_CANCEL_OBSERVED window=window-1" in compute
        assert "SPEC210_USER_CANCEL_ACK_QUEUED window=window-1 authority_revoked=true" in compute
        assert "SPEC210_CANCEL_ACK_CONFIRMED window=1" in ground_station
        assert "SPEC210_USER_CANCEL_FAILURE_SUPPRESSED" in compute
        assert "SPEC191_TRACKING_CANCELLED window=1 accepted=true" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=1 status=success" not in ground_station
        transactions = list((output / "private/compute/worker/transactions").glob("*/"))
        assert len(transactions) == 1
        assert (transactions[0] / "window-0/manifest.json").is_file()
        assert not (transactions[0] / "window-1/manifest.json").exists()
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-cancelled"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_completes_active_reference_fetch_after_source_recovery():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "recover-active-reference-fetch"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "1",
             "--global-deadline-ms", "20000", "--fault", "recover-active-reference-fetch",
             "--observation-mode", "live-push", "--headless", "--run",
             "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        source = (output / "uav3-camera.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_ACTIVE_REFERENCE_SOURCE_HELD camera=UAV3 window=window-0 category=POSE" in source
        assert "SPEC210_ACTIVE_REFERENCE_SOURCE_RECOVERED camera=UAV3 window=window-0 category=POSE" in source
        assert "SPEC210_ACTIVE_REFERENCE_FETCH_STARTED camera=UAV3 window=window-0 category=POSE" in compute
        assert "SPEC210_ACTIVE_REFERENCE_FETCH_COMPLETED camera=UAV3 window=window-0 category=POSE" in compute
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-result"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LIFECYCLE_FAULTS") != "1",
                    reason="set NDNSF_RUN_MININDN_LIFECYCLE_FAULTS=1 for MiniNDN lifecycle faults")
def test_minindn_dropped_result_times_out_terminally():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "dropped-result"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0",
             "--window-count", "3", "--global-deadline-ms", "60000",
             "--fault", "late-response-loss", "--observation-mode", "live-push",
             "--headless", "--run", "--run-seconds", "120"],
            check=False, capture_output=True, text=True, timeout=180)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr
        compute = (output / "compute.log").read_text(errors="replace")
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC191_TRACKING_RESPONSE_DROPPED window=window-2 fault=late-response-loss" in compute
        assert "SPEC191_TRACKING_TIMEOUT window=2" in ground_station
        assert "SPEC191_TRACKING_WINDOW_RESULT window=2 status=success" not in ground_station
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-timeout"


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_EXTERNAL_SOURCE") != "1",
                    reason="set NDNSF_RUN_MININDN_EXTERNAL_SOURCE=1 for the external-FD MiniNDN acceptance")
def test_minindn_external_fd_adapter_delivers_and_switches_source_generation():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        output = root / "external-fd"
        profile = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
                             .read_text(encoding="utf-8"))
        # Exercise the real external input boundary with a clearly simulated,
        # unqualified source. The reference clips supply pixels only; the profile
        # supplies simulated calibration/pose and must not imply measured truth.
        profile["acquisitionKind"] = "simulated"
        profile["geometryValidity"] = "illustrative"
        profile["accuracyQualification"] = "unvalidated"
        profile["source"] = {"kind": "external-fd"}
        profile_path = root / "external-source-profile.json"
        profile_path.write_text(json.dumps(profile), encoding="utf-8")

        tracking_libexec = Path(os.environ.get(
            "NDNSF_UAV_TRACKING_LIBEXEC", "/usr/local/libexec/ndnsf-uav/tracking"))
        adapter_body = root / "external-source-adapter.py"
        adapter_body.write_text(textwrap.dedent(f"""\
            import argparse
            import hashlib
            import os
            from pathlib import Path
            import sys
            import time
            import cv2
            assets = Path({str(ASSETS)!r})
            sys.path.insert(0, {str(tracking_libexec)!r})
            from observation_source import (load_observation_profile,
                                            load_source_generation_manifest,
                                            profile_events, source_generation_for_window,
                                            write_event)
            parser = argparse.ArgumentParser()
            for name in ("camera", "profile", "event-stream-dir", "source-id",
                         "source-generations", "world-frame-id"):
                parser.add_argument("--" + name, required=True)
            parser.add_argument("--source-epoch", type=int, required=True)
            parser.add_argument("--sample-rate-hz", type=float, required=True)
            parser.add_argument("--window-count", type=int, required=True)
            args = parser.parse_args()
            profile = load_observation_profile(Path(args.profile))
            profile["deliveryMode"] = "live-push"
            profile["sampleRateHz"] = args.sample_rate_hz
            profile["frameCount"] = args.window_count
            generation_path = Path(args.source_generations)
            generations = load_source_generation_manifest(generation_path, args.window_count)
            digest = hashlib.sha256(generation_path.read_bytes()).hexdigest()
            print("UAV_OBSERVATION_SOURCE_ADAPTER_READY "
                  "generation-manifest-sha256=sha256:" + digest, flush=True)
            video = assets / ("uav" + args.camera[-1] + "_1min.mp4")
            capture = cv2.VideoCapture(str(video))
            if not capture.isOpened():
                raise RuntimeError("cannot open simulated reference frame")
            for window in range(args.window_count):
                if os.environ.get("NDNSF_TEST_EXTERNAL_ADAPTER_STOP_BEFORE_WINDOW") == str(window):
                    break
                if os.environ.get("NDNSF_TEST_EXTERNAL_ADAPTER_PAUSE_BEFORE_WINDOW") == str(window):
                    pause_ms = int(os.environ.get(
                        "NDNSF_TEST_EXTERNAL_ADAPTER_PAUSE_MS", "250"))
                    if pause_ms <= 0 or pause_ms > 2000:
                        raise ValueError("external adapter pause must be between 1 and 2000 ms")
                    print(f"UAV_OBSERVATION_SOURCE_ADAPTER_PAUSE_BEGIN "
                          f"window={{window}} duration_ms={{pause_ms}}", flush=True)
                    time.sleep(pause_ms / 1000.0)
                    print(f"UAV_OBSERVATION_SOURCE_ADAPTER_PAUSE_RESUMED "
                          f"window={{window}}", flush=True)
                ok, frame = capture.read()
                if not ok:
                    raise RuntimeError("cannot decode simulated reference frame")
                ok, encoded = cv2.imencode(".jpg", frame)
                if not ok:
                    raise RuntimeError("cannot encode simulated reference frame")
                generation = source_generation_for_window(generations, window)
                source_id = generation["sourceIds"][args.camera]
                epoch = generation["sourceEpoch"]
                capture_us = int(round(window * 1_000_000 / args.sample_rate_hz))
                fifo = Path(args.event_stream_dir) / f"window-{{window}}.jsonl"
                with fifo.open("wb", buffering=0) as stream:
                    for kind, payload in profile_events(
                            profile, args.camera, window + 1, capture_us,
                            encoded.tobytes(), source_id=source_id, epoch=epoch,
                            image_width=frame.shape[1], image_height=frame.shape[0]):
                        if (kind == "frame" and window == 1 and args.camera == os.environ.get(
                                "NDNSF_TEST_EXTERNAL_ADAPTER_GAP_CAMERA")):
                            # Declare a source-owned capture loss before submitting
                            # the frame; this JSONL API has no admission ACK channel.
                            write_event(stream, "gap", {{
                                "schema": "uav-frame-gap-v2",
                                "identity": {{
                                    "sourceId": source_id,
                                    "droneId": profile["cameras"][args.camera]["droneId"],
                                    "cameraId": args.camera, "epoch": epoch,
                                    "worldFrameId": profile["worldFrameId"],
                                    "acquisitionKind": profile["acquisitionKind"],
                                    "deliveryMode": profile["deliveryMode"],
                                }},
                                "firstSequence": window + 1, "lastSequence": window + 1,
                                "reason": "CAPTURE_DROPPED",
                            }})
                            if os.environ.get("NDNSF_TEST_EXTERNAL_ADAPTER_INVALID_AFTER_GAP") == "1":
                                stream.write(bytes([123, 125, 10]))  # invalid empty event object
                            continue
                        if (kind == "frame" and args.camera == os.environ.get(
                                "NDNSF_TEST_EXTERNAL_ADAPTER_CLOCK_FAILURE_CAMERA")):
                            payload["clockBinding"]["clockEpoch"] += 1
                        write_event(stream, kind, payload)
                    if args.camera == os.environ.get("NDNSF_TEST_EXTERNAL_ADAPTER_OVERLOAD_CAMERA"):
                        for extra_sequence in (args.window_count + 1, args.window_count + 2):
                            for kind, payload in profile_events(
                                    profile, args.camera, extra_sequence, capture_us,
                                    encoded.tobytes(), source_id=source_id, epoch=epoch,
                                    image_width=frame.shape[1], image_height=frame.shape[0]):
                                write_event(stream, kind, payload)
                print(f"UAV_OBSERVATION_SOURCE_ADAPTER_WINDOW_COMPLETE "
                      f"window={{window}} source_id={{source_id}} epoch={{epoch}}", flush=True)
            capture.release()
            print("UAV_OBSERVATION_SOURCE_ADAPTER_COMPLETE", flush=True)
            """), encoding="utf-8")
        adapter = root / "external-source-adapter"
        adapter.write_text(
            "#!/bin/sh\nexec " + shlex.quote(sys.executable) + " " +
            shlex.quote(str(adapter_body)) + " \"$@\"\n", encoding="utf-8")
        adapter.chmod(0o755)

        pause_env = os.environ.copy()
        pause_env["NDNSF_TEST_EXTERNAL_ADAPTER_PAUSE_BEFORE_WINDOW"] = "1"
        pause_env["NDNSF_TEST_EXTERNAL_ADAPTER_PAUSE_MS"] = "250"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--observation-profile", str(profile_path),
             "--observation-mode", "live-push", "--external-source-adapter", str(adapter),
             "--window-count", "2", "--source-switch-after-window", "1",
             "--global-deadline-ms", "120000", "--headless", "--run", "--run-seconds", "300"],
            check=False, capture_output=True, text=True, timeout=360, env=pause_env)
        failure_logs = ""
        if result.returncode != 0:
            log_tails = {}
            for log_path in sorted(output.glob("*.log")):
                lines = log_path.read_text(errors="replace").splitlines()
                log_tails[log_path.name] = "\n".join(lines[-12:])
            failure_logs = "\nexternal-FD run log tails:\n" + json.dumps(log_tails, indent=2)
        assert result.returncode == 0, result.stdout + "\n" + result.stderr + failure_logs
        manifest = json.loads((output / "manifest.json").read_text())
        observation = manifest["observation"]
        assert observation["source"]["kind"] == "external-fd"
        assert observation["acquisitionKind"] == "simulated"
        assert observation["accuracyQualification"] == "unvalidated"
        generations = observation["sourceGenerations"]["generations"]
        assert [item["firstWindowIndex"] for item in generations] == [0, 1]
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_SOURCE_GENERATION_ACTIVATED first_window=1" in ground_station
        assert ground_station.count("status=success response_status=success") == 2
        lifecycle = json.loads((output / "lifecycle.json").read_text())
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-result"
        for camera in ("UAV1", "UAV2", "UAV3"):
            adapter_log = (output / f"external-source-{camera.lower()}.log").read_text(
                errors="replace")
            assert "UAV_OBSERVATION_SOURCE_ADAPTER_READY" in adapter_log
            assert "generation-manifest-sha256=sha256:" in adapter_log
            assert ("UAV_OBSERVATION_SOURCE_ADAPTER_PAUSE_BEGIN "
                    "window=1 duration_ms=250") in adapter_log
            assert "UAV_OBSERVATION_SOURCE_ADAPTER_PAUSE_RESUMED window=1" in adapter_log
            assert "UAV_OBSERVATION_SOURCE_ADAPTER_COMPLETE" in adapter_log
        results = output / "private/compute/worker/results"
        for index in range(2):
            result_json = json.loads((results / f"window-{index}.json").read_text())
            generation = generations[index]
            assert result_json["terminal"] is True
            assert [frame["sourceId"] for frame in result_json["frames"]] == [
                generation["sourceIds"][camera] for camera in ("UAV1", "UAV2", "UAV3")]
            assert {frame["sourceEpoch"] for frame in result_json["frames"]} == {
                generation["sourceEpoch"]}

        # A source process can exit cleanly after its first window while the
        # application is still running. The next missing window must hit the
        # bounded stream deadline and produce a terminal failure, not hang.
        lost_output = root / "external-fd-source-loss"
        source_loss_env = os.environ.copy()
        source_loss_env["NDNSF_TEST_EXTERNAL_ADAPTER_STOP_BEFORE_WINDOW"] = "1"
        lost = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(lost_output),
             "--license", "Apache-2.0", "--observation-profile", str(profile_path),
             "--observation-mode", "live-push", "--external-source-adapter", str(adapter),
             "--window-count", "2", "--source-switch-after-window", "1",
             "--global-deadline-ms", "120000", "--headless", "--run", "--run-seconds", "300"],
            check=False, capture_output=True, text=True, timeout=360, env=source_loss_env)
        assert lost.returncode == 1, lost.stdout + "\n" + lost.stderr
        failed_sources = "\n".join(
            (lost_output / f"uav{index}-camera.log").read_text(errors="replace")
            for index in (1, 2, 3))
        lost_lifecycle = json.loads((lost_output / "lifecycle.json").read_text())
        assert "observation stream exceeded its source-age deadline" in failed_sources
        assert lost_lifecycle["terminal"] is True
        assert lost_lifecycle["outcome"] == "collaboration-failed"

        # A frame/pose clock-epoch mismatch must keep detection available while
        # making geometry-dependent measurements explicitly unknown.
        clock_output = root / "external-fd-clock-failure"
        clock_failure_env = os.environ.copy()
        clock_failure_env["NDNSF_TEST_EXTERNAL_ADAPTER_CLOCK_FAILURE_CAMERA"] = "UAV3"
        clock_failure = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(clock_output),
             "--license", "Apache-2.0", "--observation-profile", str(profile_path),
             "--observation-mode", "live-push", "--external-source-adapter", str(adapter),
             "--window-count", "1", "--global-deadline-ms", "120000",
             "--headless", "--run", "--run-seconds", "300"],
            check=False, capture_output=True, text=True, timeout=360, env=clock_failure_env)
        assert clock_failure.returncode == 0, clock_failure.stdout + "\n" + clock_failure.stderr
        clock_lifecycle = json.loads((clock_output / "lifecycle.json").read_text())
        assert clock_lifecycle["terminal"] is True
        assert clock_lifecycle["outcome"] == "collaboration-result"
        clock_result = json.loads(
            (clock_output / "private/compute/worker/results/window-0.json").read_text())
        uav3 = next(frame for frame in clock_result["frames"] if frame["cameraId"] == "UAV3")
        assert uav3["tracks"]
        assert uav3["measurements"]
        assert all(item["rangeStatus"] == "unknown" for item in uav3["measurements"])
        assert all("POSE_UNAVAILABLE" in item["reasonCodes"] for item in uav3["measurements"])

        # Three frames in one source window exceed the configured two-frame
        # ingress bound. The source must reject the burst explicitly and fail
        # the collaboration without accepting an unbounded backlog.
        overload_output = root / "external-fd-overload"
        overload_env = os.environ.copy()
        overload_env["NDNSF_TEST_EXTERNAL_ADAPTER_OVERLOAD_CAMERA"] = "UAV3"
        overload = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(overload_output),
             "--license", "Apache-2.0", "--observation-profile", str(profile_path),
             "--observation-mode", "live-push", "--external-source-adapter", str(adapter),
             "--window-count", "1", "--global-deadline-ms", "120000",
             "--headless", "--run", "--run-seconds", "300"],
            check=False, capture_output=True, text=True, timeout=360, env=overload_env)
        assert overload.returncode == 1, overload.stdout + "\n" + overload.stderr
        overload_source = (overload_output / "uav3-camera.log").read_text(errors="replace")
        overload_lifecycle = json.loads((overload_output / "lifecycle.json").read_text())
        assert "observation stream rejected an event: QUEUE_FULL" in overload_source
        assert overload_lifecycle["terminal"] is True
        assert overload_lifecycle["outcome"] == "collaboration-failed"

        # A declared capture loss is distinct from rejected queue admission.
        # Requesting that exact key must fail explicitly, with no replacement
        # image or successful second result entering the compute worker.
        for invalid_after_gap in (False, True):
            gap_output = root / ("external-fd-declared-gap-invalid" if invalid_after_gap
                                     else "external-fd-declared-gap")
            gap_env = os.environ.copy()
            gap_env["NDNSF_TEST_EXTERNAL_ADAPTER_GAP_CAMERA"] = "UAV3"
            if invalid_after_gap:
                gap_env["NDNSF_TEST_EXTERNAL_ADAPTER_INVALID_AFTER_GAP"] = "1"
            gap_run = subprocess.run(
                [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
                 "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(gap_output),
                 "--license", "Apache-2.0", "--observation-profile", str(profile_path),
                 "--observation-mode", "live-push", "--external-source-adapter", str(adapter),
                 "--window-count", "2", "--global-deadline-ms", "120000",
                 "--headless", "--run", "--run-seconds", "300"],
                check=False, capture_output=True, text=True, timeout=360, env=gap_env)
            assert gap_run.returncode == 1, gap_run.stdout + "\n" + gap_run.stderr
            gap_lifecycle = json.loads((gap_output / "lifecycle.json").read_text())
            assert gap_lifecycle["terminal"] is True
            assert gap_lifecycle["outcome"] == "collaboration-failed"
            gap_manifest = json.loads((gap_output / "manifest.json").read_text())
            gap_generation = gap_manifest["observation"]["sourceGenerations"]["generations"][0]
            gap_source = (gap_output / "uav3-camera.log").read_text(errors="replace")
            records = [json.loads(line)["gap"] for line in gap_source.splitlines()
                       if line.startswith('{"event":"SOURCE_FRAME_GAP",')]
            assert records == [{
                "schema": "uav-frame-gap-v2",
                "identity": {
                    "sourceId": gap_generation["sourceIds"]["UAV3"],
                    "droneId": profile["cameras"]["UAV3"]["droneId"],
                    "cameraId": "UAV3", "epoch": str(gap_generation["sourceEpoch"]),
                    "worldFrameId": profile["worldFrameId"],
                    "acquisitionKind": "simulated", "deliveryMode": "live-push",
                },
                "firstSequence": "2", "lastSequence": "2", "reason": "CAPTURE_DROPPED",
            }]
            expected_error = ("observation stream rejected an event: INVALID_EVENT" if invalid_after_gap
                              else "observation stream did not produce a ready frame: FRAME_GAP")
            assert expected_error in gap_source
            gap_compute = (gap_output / "compute.log").read_text(errors="replace")
            assert "SPEC210_TRACKING_WORKER_INVOKED window=window-0" in gap_compute
            assert "SPEC210_TRACKING_WORKER_INVOKED window=window-1" not in gap_compute
            committed = sorted((gap_output / "private/compute/worker/results").glob("window-*.json"))
            assert [path.name for path in committed] == ["window-0.json"]
            first_result = json.loads(committed[0].read_text())
            assert {frame["sequence"] for frame in first_result["frames"]} == {1}


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_LEGACY_NO_MOTION") != "1",
                    reason="set NDNSF_RUN_MININDN_LEGACY_NO_MOTION=1 for the legacy compatibility acceptance")
def test_minindn_legacy_no_motion_cli_path_completes_without_motion_measurements():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    trust_schema_value = os.environ.get("NDNSF_UAV_DEMO_TEST_TRUST_SCHEMA", "")
    trust_schema = Path(trust_schema_value).expanduser() if trust_schema_value else None
    if trust_schema is None or not trust_schema.is_file():
        pytest.skip("set NDNSF_UAV_DEMO_TEST_TRUST_SCHEMA to the isolated test-CA trust schema")

    with tempfile.TemporaryDirectory(prefix="spec191-legacy-no-motion-") as directory:
        output = Path(directory) / "legacy-no-motion"
        result = subprocess.run(
            [sys.executable, str(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--window-count", "3", "--legacy-mode",
             "--trust-schema", str(trust_schema), "--headless", "--run", "--run-seconds", "180"],
            check=False, capture_output=True, text=True, timeout=300)
        assert result.returncode == 0, result.stdout + "\n" + result.stderr

        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["motion"] == {"mode": "disabled", "reason": "legacy-no-motion"}
        assert manifest["observation"] == {"enabled": False, "mode": "legacy"}
        for process in manifest["processes"]:
            assert "--motion-dir" not in process["command"]
            assert "--observation-stream-dir" not in process["command"]
        ground_station = (output / "ground-station.log").read_text(errors="replace")
        window_results = [line for line in ground_station.splitlines()
                          if line.startswith("SPEC191_TRACKING_WINDOW_RESULT")]
        assert len(window_results) == 3
        assert all("status=success" in line and "result_verified=true" in line
                   for line in window_results)
        assert "SPEC191_TRACKING_RESULT status=success window=2 windows=3" in ground_station

        lifecycle = json.loads((output / "lifecycle.json").read_text(encoding="utf-8"))
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-result"
        display_lines = (output / "compute-display.log").read_text(errors="replace").splitlines()
        shown = []
        for line in display_lines:
            if line.startswith("{"):
                event = json.loads(line)
                if event.get("event") == "DISPLAY_FRAME_SHOWN":
                    shown.append(event)
        assert len(shown) == 9
        assert all(item["measurementCount"] == 0 and item["motionStatus"] == "unknown"
                   and item["provenance"] == "legacy" for item in shown)
