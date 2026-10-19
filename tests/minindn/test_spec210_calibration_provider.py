from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[2]
ASSETS = Path(os.environ.get(
    "NDNSF_UAV_DEMO_ASSET_DIR", "/home/tianxing/.cache/ndnsf/spec191-assets")).expanduser()
MODEL = Path(os.environ.get("NDNSF_UAV_DEMO_MODEL", str(ASSETS / "3UAVs.pt"))).expanduser()
TRUST_SCHEMA_VALUE = os.environ.get("NDNSF_UAV_DEMO_TEST_TRUST_SCHEMA", "")
TRUST_SCHEMA = Path(TRUST_SCHEMA_VALUE).expanduser() if TRUST_SCHEMA_VALUE else None
PROVIDER_HELPER_VALUE = os.environ.get("NDNSF_SPEC210_CALIBRATION_PROVIDER_HELPER", "")
PROVIDER_HELPER = Path(PROVIDER_HELPER_VALUE).expanduser() if PROVIDER_HELPER_VALUE else None


@pytest.mark.skipif(os.environ.get("NDNSF_RUN_MININDN_EXTERNAL_PROVIDER") != "1",
                    reason="set NDNSF_RUN_MININDN_EXTERNAL_PROVIDER=1 for the selected-provider MiniNDN regression")
def test_minindn_selected_provider_rejects_authorized_camera_calibration():
    if os.geteuid() != 0:
        pytest.skip("MiniNDN requires root")
    if not (ASSETS / "uav1_1min.mp4").is_file() or not MODEL.is_file():
        pytest.skip("Spec191 video/model assets unavailable")
    if TRUST_SCHEMA is None or not TRUST_SCHEMA.is_file():
        pytest.skip("set NDNSF_UAV_DEMO_TEST_TRUST_SCHEMA to the isolated test-CA trust schema")
    if PROVIDER_HELPER is None or not PROVIDER_HELPER.is_file():
        pytest.skip("set NDNSF_SPEC210_CALIBRATION_PROVIDER_HELPER to the built Spec210 test provider")

    wrapper = ROOT / "tests/minindn/fixtures/run_spec210_external_provider.py"
    adapter = ROOT / "tests/minindn/fixtures/spec210-external-calibration-source.py"
    with tempfile.TemporaryDirectory(prefix="s210-provider-") as directory:
        temporary_root = Path(directory)
        output = temporary_root / "run"
        profile = json.loads(
            (ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text(
                encoding="utf-8"))
        profile["source"] = {"kind": "external-fd"}
        profile_path = temporary_root / "external-source-profile.json"
        profile_path.write_text(json.dumps(profile), encoding="utf-8")
        environment = os.environ.copy()
        environment["SPEC210_T012_FAULT"] = "authorized-camera-producer"
        result = subprocess.run(
            [sys.executable, str(wrapper),
             "--source", str(ASSETS), "--model", str(MODEL), "--output-dir", str(output),
             "--license", "Apache-2.0", "--trust-schema", str(TRUST_SCHEMA),
             "--observation-profile", str(profile_path),
             "--observation-mode", "live-push", "--external-source-adapter", str(adapter),
             "--window-count", "2", "--global-deadline-ms", "120000",
             "--headless", "--run", "--run-seconds", "300"],
            check=False, capture_output=True, text=True, timeout=360, env=environment)
        assert result.returncode == 1, result.stdout + "\n" + result.stderr

        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        provider_process = next(item for item in manifest["processes"]
                                if item["name"] == "external-source-calibration-provider")
        assert provider_process["identity"] == "/example/uav/calibration-provider"
        ground_station_command = next(item["command"] for item in manifest["processes"]
                                       if item["name"] == "ground-station")
        assert ground_station_command[-2:] == [
            "--calibration-provider", "/example/uav/calibration-provider"]

        expected_logs = ("external-source-calibration-provider.log",
                         "external-source-uav3.log", "compute.log", "ground-station.log")
        missing_logs = [name for name in expected_logs if not (output / name).is_file()]
        created_log_tails = {
            item.name: "\n".join(item.read_text(errors="replace").splitlines()[-24:])
            for item in output.glob("*.log")
        }
        assert not missing_logs, (
            f"MiniNDN did not create expected process logs {missing_logs}; "
            f"log tails={json.dumps(created_log_tails, indent=2)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        provider_log = (output / "external-source-calibration-provider.log").read_text(
            errors="replace")
        source_log = (output / "external-source-uav3.log").read_text(errors="replace")
        compute_log = (output / "compute.log").read_text(errors="replace")
        ground_station_log = (output / "ground-station.log").read_text(errors="replace")
        assert "SPEC210_CAMERA_CALIBRATION_INJECTED camera=UAV3 window=window-1" in source_log, (
            f"the source adapter did not reach the target frame\n"
            f"source adapter:\n{source_log[-4000:]}\n"
            f"UAV3 camera:\n{(output / 'uav3-camera.log').read_text(errors='replace')[-4000:]}\n"
            f"compute:\n{compute_log[-4000:]}\n"
            f"ground station:\n{ground_station_log[-4000:]}")
        assert provider_log.count("SPEC210_EXTERNAL_CALIBRATION_ROLE_COMPLETE invocation=") >= 1
        assert "CALIBRATION reference is not owned by the selected Calibration-Provider" in compute_log
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-0" in compute_log
        assert "SPEC210_TRACKING_WORKER_INVOKED window=window-1" not in compute_log
        assert ("SPEC191_TRACKING_SELECTED role=Calibration-Provider "
                "provider=/example/uav/calibration-provider") in ground_station_log
        assert "SPEC191_TRACKING_WINDOW_RESULT window=0 status=success" in ground_station_log
        assert "SPEC191_TRACKING_WINDOW_RESULT window=1 status=failure" in ground_station_log

        lifecycle = json.loads((output / "lifecycle.json").read_text(encoding="utf-8"))
        assert lifecycle["terminal"] is True
        assert lifecycle["outcome"] == "collaboration-failed"
