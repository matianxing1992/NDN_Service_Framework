from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from Experiments.UAV.run_multicamera_tracking_demo import (
    ProcessSupervisor,
    RunnerError,
    TOPOLOGY,
    _has_all_markers_on_one_line,
    _minindn_space_delimiter_compat,
    _source_generation_ready_marker,
    _snapshot_observation_profile,
    _write_json,
    _child_pythonpath,
    _application_library_environment_command,
    build_plan,
    main,
    preflight,
)
from Experiments.UAV.tracking_artifacts import validate_video_reference_assets


ASSETS = Path(os.environ.get(
    "NDNSF_UAV_DEMO_ASSET_DIR", str(Path.home() / ".cache/ndnsf/spec191-assets")))
MODEL = Path(os.environ.get("NDNSF_UAV_DEMO_MODEL", str(ASSETS / "3UAVs.pt")))


class TrackingRunnerTest(unittest.TestCase):
    def test_default_application_loader_removes_inherited_library_path(self) -> None:
        with patch.dict(os.environ, {"LD_LIBRARY_PATH": "/tmp/obsolete-library-root"}):
            os.environ.pop("NDNSF_UAV_LIBRARY_PATH", None)
            result = subprocess.run(
                ["/bin/sh", "-c", _application_library_environment_command() +
                 "; exec /usr/bin/env -0"], check=True, capture_output=True,
                env=dict(os.environ))
        child = dict(item.split(b"=", 1) for item in result.stdout.split(b"\0") if item)
        self.assertNotIn(b"LD_LIBRARY_PATH", child)

    def test_explicit_application_library_override_is_exported_literally(self) -> None:
        for override in ("", "/usr/local/SDK with spaces/lib:/usr/local/lib",
                         "/usr/local/SDK 'literal'; $(printf not-executed)/lib:$ORIGIN"):
            with self.subTest(override=override), patch.dict(os.environ, {
                    "LD_LIBRARY_PATH": "/tmp/obsolete-library-root",
                    "NDNSF_UAV_LIBRARY_PATH": override}):
                result = subprocess.run(
                    ["/bin/sh", "-c", _application_library_environment_command() +
                     "; exec /usr/bin/env -0"], check=True, capture_output=True,
                    env=dict(os.environ))
                child = dict(item.split(b"=", 1) for item in result.stdout.split(b"\0") if item)
                self.assertEqual(child[b"LD_LIBRARY_PATH"], override.encode())

    def test_window_count_rejects_noninteger_api_values(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for count in (True, False, 1.5, 1.0, "3"):
                with self.subTest(count=count), self.assertRaisesRegex(RunnerError, "integer"):
                    build_plan(source=root, model=root / "unused.pt", output=root / "output",
                               license_text="test", window_count=count)
            self.assertFalse((root / "output").exists())

    def test_invalid_profile_counts_reject_before_runtime_overrides(self) -> None:
        original = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "model.pt"
            model.write_bytes(b"test-model")
            profile = root / "profile.json"
            for field, value in (("frameCount", True), ("frameCount", 1.5),
                                 ("sampleRateHz", True), ("source", {"kind": []}),
                                 ("source", {"kind": {}}), ("schema", []),
                                 ("acquisitionKind", {}), ("geometryValidity", [])):
                profile.write_text(json.dumps({**original, field: value}))
                with self.subTest(field=field, value=value), \
                        self.assertRaisesRegex(RunnerError, "observation profile is invalid"):
                    build_plan(source=root, model=model, output=root / "output",
                               license_text="test", observation_profile=profile, window_count=3)

    def test_same_output_new_plans_have_distinct_source_epoch_scopes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "model.pt"
            model.write_bytes(b"test-model")
            source = root / "source"
            source.mkdir()
            with patch("Experiments.UAV.run_multicamera_tracking_demo.validate_real_assets",
                       return_value={"testAssets": True}), \
                 patch("Experiments.UAV.run_multicamera_tracking_demo.secrets.randbits", side_effect=[7, 8]):
                plans = [build_plan(source=source, model=model, output=root / "same-output",
                                    license_text="test", headless=True) for _ in range(2)]
            self.assertEqual(plans[0]["observation"]["sourceIds"], plans[1]["observation"]["sourceIds"])
            self.assertEqual([plan["observation"]["sourceEpoch"] for plan in plans], [7, 8])
            for plan in plans:
                self.assertEqual(plan["observation"]["sourceGenerations"]["generations"][0]["sourceEpoch"],
                                 plan["observation"]["sourceEpoch"])

    def test_profile_migration_preserves_raw_and_effective_snapshot_identities(self) -> None:
        original = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text())
        effective_profiles = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "model.pt"
            model.write_bytes(b"test-model")
            source = root / "source"
            source.mkdir()
            for version in ("uav-observation-demo-profile-v1", "uav-observation-profile-v2"):
                with self.subTest(version=version):
                    original["schema"] = version
                    raw = (json.dumps(original, indent=3) + "\n\n").encode()
                    profile_path = root / "profile.json"
                    profile_path.write_bytes(raw)
                    output = root / version
                    with patch("Experiments.UAV.run_multicamera_tracking_demo.validate_real_assets",
                               return_value={"testAssets": True}):
                        plan = build_plan(source=source, model=model, output=output,
                                          license_text="test", headless=True,
                                          observation_profile=profile_path)
                    observation = plan["observation"]
                    effective = _snapshot_observation_profile(observation, output)
                    effective_profiles.append(effective)
                    self.assertEqual(Path(observation["rawProfileSnapshot"]).read_bytes(), raw)
                    self.assertEqual(profile_path.read_bytes(), raw)
                    self.assertEqual(observation["profileDigest"],
                                     "sha256:" + hashlib.sha256(raw).hexdigest())
                    expected = dict(original, schema="uav-observation-profile-v2",
                                    sampleRateHz=5.0, frameCount=100, deliveryMode="replay")
                    effective_bytes = (json.dumps(expected, indent=2, sort_keys=True) + "\n").encode()
                    self.assertEqual(effective, expected)
                    self.assertEqual(Path(observation["effectiveProfileSnapshot"]).read_bytes(),
                                     effective_bytes)
                    self.assertEqual(observation["effectiveProfileDigest"],
                                     "sha256:" + hashlib.sha256(effective_bytes).hexdigest())
                    profile_path.write_bytes(raw + b" ")
                    with self.assertRaisesRegex(RunnerError, "profile changed"):
                        _snapshot_observation_profile(observation, output)
                    profile_path.write_bytes(raw)
                    tampered = dict(observation, effectiveProfileDigest="sha256:" + "0" * 64)
                    with self.assertRaisesRegex(RunnerError, "effective observation profile"):
                        _snapshot_observation_profile(tampered, output)
            self.assertEqual(effective_profiles[0], effective_profiles[1])

    def test_cutover_ready_markers_must_share_the_ready_line(self) -> None:
        self.assertTrue(_has_all_markers_on_one_line(
            "UAV_OBSERVATION_SOURCE_ADAPTER_READY generation-manifest-sha256=sha256:abc",
            ("UAV_OBSERVATION_SOURCE_ADAPTER_READY", "generation-manifest-sha256=sha256:abc")))
        self.assertFalse(_has_all_markers_on_one_line(
            "UAV_OBSERVATION_SOURCE_ADAPTER_READY\ngeneration-manifest-sha256=sha256:abc",
            ("UAV_OBSERVATION_SOURCE_ADAPTER_READY", "generation-manifest-sha256=sha256:abc")))

    def test_atomic_json_writer_creates_private_parent_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "private" / "config" / "profile.json"
            _write_json(target, {"schema": "test-v1"})

            self.assertEqual(json.loads(target.read_text(encoding="utf-8")),
                             {"schema": "test-v1"})
            self.assertFalse(target.with_suffix(".json.tmp").exists())

    def test_minindn_space_delimited_topology_parser_compatibility(self) -> None:
        import configparser

        original_parser = configparser.ConfigParser
        with _minindn_space_delimiter_compat():
            parser = configparser.ConfigParser(delimiters=" ", allow_no_value=True)
            parser.read(TOPOLOGY)
            self.assertEqual(dict(parser.items("links"))["uav1:compute"],
                             "delay=2ms bw=100 loss=0")
            self.assertEqual(dict(parser.items("faces"))["uav1:compute"], "cost=10")
        self.assertIs(configparser.ConfigParser, original_parser)

    def test_spec210_default_plan_is_100_frames_at_five_hz_without_legacy_motion(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            output = root / "output"
            with patch("Experiments.UAV.run_multicamera_tracking_demo.validate_real_assets",
                       return_value={"testAssets": True}):
                plan = build_plan(source=source, model=model, output=output,
                                  license_text="local-test-accepted", headless=True)

        self.assertEqual(plan["settings"]["windowCount"], 100)
        self.assertEqual(plan["settings"]["sampleFps"], 5.0)
        self.assertEqual(plan["observation"]["mode"], "simulated")
        self.assertEqual(plan["observation"]["sourceGenerations"]["schema"],
                         "uav-source-generations-v1")
        self.assertEqual(len(plan["observation"]["sourceGenerations"]["generations"]), 1)
        self.assertIsNone(_source_generation_ready_marker(plan["observation"]))
        self.assertFalse(plan["scientificAccuracyClaimAllowed"])
        self.assertFalse(plan["motion"]["mode"] == "legacy")
        self.assertEqual(len(plan["processes"]), 7)
        display = next(item for item in plan["processes"] if item["name"] == "compute-display")
        self.assertIn("--playback-clock-manifest", display["command"])
        self.assertIn("--playback-clock-input-dir", display["command"])
        self.assertIn("--observation-profile", display["command"])
        self.assertIn("--source-generations", display["command"])
        compute = next(item for item in plan["processes"] if item["name"] == "compute")
        self.assertIn("--worker-python", compute["command"])
        self.assertEqual(compute["command"][compute["command"].index("--worker-python") + 1],
                         sys.executable)
        for camera in ("UAV1", "UAV2", "UAV3"):
            process = next(item for item in plan["processes"]
                           if item["name"] == f"uav{camera[-1]}-camera")
            self.assertIn("--observation-stream-dir", process["command"])
            self.assertIn("--source-epoch", process["command"])
            self.assertIn("--source-generations", process["command"])
            self.assertNotIn("--motion-dir", process["command"])

    def test_trust_schema_is_pinned_and_passed_to_every_native_ndn_role(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            trust_schema = root / "deployment-trust.conf"
            trust_bytes = b"trust-anchor { type any }\n"
            trust_schema.write_bytes(trust_bytes)
            with patch("Experiments.UAV.run_multicamera_tracking_demo.validate_real_assets",
                       return_value={"testAssets": True}):
                plan = build_plan(
                    source=source, model=model, output=root / "output",
                    license_text="local-test-accepted", headless=True,
                    trust_schema=trust_schema)

        self.assertEqual(plan["security"]["trustSchema"], str(trust_schema.resolve()))
        self.assertEqual(plan["security"]["trustSchemaDigest"],
                         "sha256:" + hashlib.sha256(trust_bytes).hexdigest())
        roles = {"controller", "ground-station", "uav1-camera", "uav2-camera",
                 "uav3-camera", "compute"}
        for process in plan["processes"]:
            if process["name"] not in roles:
                continue
            command = process["command"]
            self.assertIn("--trust-schema", command, process["name"])
            self.assertEqual(command[command.index("--trust-schema") + 1],
                             str(trust_schema.resolve()))

    def test_spec210_live_push_and_legacy_are_explicit_separate_plans(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            video_passport = {
                "mode": "video-reference-simulated",
                "videos": [{"cameraId": camera} for camera in ("UAV1", "UAV2", "UAV3")],
            }
            with patch("Experiments.UAV.run_multicamera_tracking_demo.validate_real_assets",
                       return_value={"testAssets": True}), \
                    patch("Experiments.UAV.run_multicamera_tracking_demo.validate_video_reference_assets",
                          return_value=video_passport) as validate_video:
                pushed = build_plan(
                    source=source, model=model, output=root / "push",
                    license_text="local-test-accepted", headless=True,
                    window_count=2, observation_mode="live-push")
                legacy = build_plan(
                    source=source, model=model, output=root / "legacy",
                    license_text="local-test-accepted", headless=True,
                    window_count=3, motion_profile="nominal")
                with self.assertRaisesRegex(RunnerError, "AMBIGUOUS_INPUT"):
                    build_plan(
                        source=source, model=model, output=root / "legacy-mixed",
                        license_text="local-test-accepted", headless=True,
                        window_count=3, motion_profile="nominal",
                        observation_profile=ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
                with self.assertRaisesRegex(RunnerError, "cannot be combined with legacy mode"):
                    build_plan(
                        source=source, model=model, output=root / "legacy-adapter",
                        license_text="local-test-accepted", headless=True,
                        window_count=3, motion_profile="nominal",
                        external_source_adapter=root / "ignored-adapter")
                with self.assertRaises(RunnerError):
                    build_plan(
                        source=source, model=model, output=root / "legacy-too-large",
                        license_text="local-test-accepted", headless=True,
                        window_count=61, motion_profile="nominal")
            validate_video.assert_called_once_with(
                source.resolve(), model.resolve(), "local-test-accepted")

        self.assertEqual(pushed["assets"], video_passport)
        pushers = [item for item in pushed["processes"]
                   if item["name"].startswith("observation-push-")]
        self.assertEqual(len(pushers), 3)
        self.assertEqual(pushed["observation"]["source"]["kind"], "video-reference")
        self.assertEqual(pushed["observation"]["source"]["adapter"], "reference-push-adapter")
        self.assertTrue(all("--fifo-directory" in item["command"] for item in pushers))
        compute = next(item for item in pushed["processes"] if item["name"] == "compute")
        source_video_paths = {
            str(source / f"uav{index}_1min.mp4") for index in (1, 2, 3)
        }
        self.assertTrue(all(
            any(video_path in item["command"] for item in pushers)
            for video_path in source_video_paths
        ))
        self.assertNotIn("--video-source", compute["command"])
        self.assertTrue(source_video_paths.isdisjoint(compute["command"]))
        for option, expected in (("--source-epoch", pushed["observation"]["sourceEpoch"]),
                                 ("--world-frame-id", "synthetic-local"),
                                 ("--acquisition-kind", "simulated"),
                                 ("--delivery-mode", "live-push")):
            self.assertEqual(compute["command"][compute["command"].index(option) + 1],
                             str(expected))
        self.assertEqual(legacy["motion"]["mode"], "legacy")
        self.assertEqual(legacy["observation"]["mode"], "legacy")
        self.assertTrue(all("--motion-dir" in item["command"]
                            for item in legacy["processes"]
                            if item["name"].endswith("-camera")))

    def test_cli_explicit_legacy_mode_preserves_no_motion_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            output = root / "legacy-no-motion"
            with patch("Experiments.UAV.run_multicamera_tracking_demo.validate_real_assets",
                       return_value={"testAssets": True}):
                result = main([
                    "--source", str(source), "--model", str(model),
                    "--output-dir", str(output), "--headless", "--prepare",
                    "--legacy-mode",
                ])

            self.assertEqual(result, 0)
            plan = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(plan["settings"]["windowCount"], 3)
            self.assertEqual(plan["motion"], {
                "mode": "disabled", "reason": "legacy-no-motion"})
            self.assertEqual(plan["observation"], {
                "enabled": False, "mode": "legacy"})
            for process in plan["processes"]:
                if process["name"] in {"ground-station", "uav1-camera", "uav2-camera",
                                        "uav3-camera", "compute"}:
                    self.assertNotIn("--motion-dir", process["command"])
                    self.assertNotIn("--observation-stream-dir", process["command"])
                    self.assertNotIn("--observation-schema", process["command"])

            with self.assertRaisesRegex(RunnerError, "observation mode"):
                build_plan(
                    source=source, model=model, output=root / "legacy-live-push",
                    license_text="local-test-accepted", headless=True,
                    legacy_mode=True, observation_mode="live-push")

    def test_spec210_source_switch_is_a_single_run_schedule_bound_to_all_roles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            with patch("Experiments.UAV.run_multicamera_tracking_demo.validate_video_reference_assets",
                       return_value={"testAssets": True}):
                plan = build_plan(
                    source=root / "videos", model=model, output=root / "run",
                    license_text="local-test-accepted", headless=True, window_count=5,
                    observation_mode="live-push", source_switch_after_window=[2, 4])
                with self.assertRaisesRegex(RunnerError, "strictly increasing"):
                    build_plan(
                        source=root / "videos", model=model, output=root / "unordered",
                        license_text="local-test-accepted", headless=True, window_count=5,
                        observation_mode="live-push", source_switch_after_window=[3, 2])
                with self.assertRaisesRegex(RunnerError, "before the final window"):
                    build_plan(
                        source=root / "videos", model=model, output=root / "late",
                        license_text="local-test-accepted", headless=True, window_count=5,
                        observation_mode="live-push", source_switch_after_window=[5])

        generations = plan["observation"]["sourceGenerations"]["generations"]
        self.assertEqual(
            _source_generation_ready_marker(plan["observation"]),
            f"generation-manifest-sha256={plan['observation']['sourceGenerationManifestSha256']}")
        self.assertEqual([item["firstWindowIndex"] for item in generations], [0, 2, 4])
        self.assertEqual([item["sourceEpoch"] for item in generations],
                         [plan["observation"]["sourceEpoch"] + offset for offset in range(3)])
        for camera in ("UAV1", "UAV2", "UAV3"):
            identities = [item["sourceIds"][camera] for item in generations]
            self.assertEqual(len(set(identities)), 3)
        generation_path = plan["observation"]["sourceGenerationManifest"]
        for name in ("ground-station", "uav1-camera", "uav2-camera", "uav3-camera",
                     "compute", "observation-push-uav1", "observation-push-uav2",
                     "observation-push-uav3"):
            process = next(item for item in plan["processes"] if item["name"] == name)
            self.assertEqual(process["command"][process["command"].index("--source-generations") + 1],
                             generation_path)

    def test_spec210_measured_external_source_uses_live_pipe_without_video_assets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            profile = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
                                 .read_text(encoding="utf-8"))
            profile["acquisitionKind"] = "measured"
            profile["geometryValidity"] = "calibrated-plane"
            profile["source"] = {"kind": "external-fd"}
            profile_path = root / "measured-source.json"
            profile_path.write_text(json.dumps(profile), encoding="utf-8")
            plan = build_plan(
                source=root / "no-video-directory", model=model,
                output=root / "measured-run", license_text="local-test-accepted",
                headless=True, window_count=2, observation_mode="live-push",
                observation_profile=profile_path)

        self.assertEqual(plan["assets"]["mode"], "live-source-model-only")
        self.assertEqual(plan["observation"]["acquisitionKind"], "measured")
        self.assertEqual(plan["observation"]["source"]["adapter"], "external-observation-fd")
        endpoints = plan["observation"]["source"]["writerEndpoints"]
        self.assertEqual(endpoints["transport"], "named-fifo-file-descriptor")
        self.assertEqual(endpoints["eventSchema"], "uav-source-event-v2")
        self.assertTrue(endpoints["closeEachWindow"])
        self.assertEqual(endpoints["writerOwner"], "external-caller")
        self.assertEqual(len(endpoints["pathsByCamera"]["UAV1"]), 2)
        self.assertTrue(all(path.endswith(f"window-{index}.jsonl")
                            for index, path in enumerate(endpoints["pathsByCamera"]["UAV1"])))
        self.assertFalse(any(item["name"].startswith("observation-push-")
                             for item in plan["processes"]))
        blockers = preflight(plan, run=True, headless=True)["errors"]
        self.assertIn("external-fd run requires --external-source-adapter to provide FIFO writers",
                      blockers)
        compute = next(item for item in plan["processes"] if item["name"] == "compute")
        for option, expected in (("--source-epoch", plan["observation"]["sourceEpoch"]),
                                 ("--world-frame-id", "synthetic-local"),
                                 ("--acquisition-kind", "measured"),
                                 ("--delivery-mode", "live-push")):
            self.assertEqual(compute["command"][compute["command"].index(option) + 1],
                             str(expected))
        for camera in ("UAV1", "UAV2", "UAV3"):
            process = next(item for item in plan["processes"]
                           if item["name"] == f"uav{camera[-1]}-camera")
            self.assertIn("--observation-stream-dir", process["command"])
            self.assertNotIn("--video-source", process["command"])

    def test_spec210_external_source_adapter_is_started_as_fifo_writer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            adapter = root / "source-adapter"
            adapter.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            adapter.chmod(0o755)
            profile = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
                                 .read_text(encoding="utf-8"))
            profile["acquisitionKind"] = "measured"
            profile["geometryValidity"] = "calibrated-plane"
            profile["source"] = {"kind": "external-fd"}
            profile_path = root / "measured-source.json"
            profile_path.write_text(json.dumps(profile), encoding="utf-8")

            plan = build_plan(
                source=root / "no-video-directory", model=model,
                output=root / "measured-run", license_text="local-test-accepted",
                headless=True, window_count=2, observation_mode="live-push",
                observation_profile=profile_path, external_source_adapter=adapter)

        endpoints = plan["observation"]["source"]["writerEndpoints"]
        self.assertEqual(endpoints["writerOwner"], "runner-source-adapter")
        display = next(item for item in plan["processes"] if item["name"] == "compute-display")
        self.assertNotIn("--playback-clock-manifest", display["command"])
        self.assertNotIn("--playback-clock-input-dir", display["command"])
        self.assertIn("--observation-profile", display["command"])
        self.assertIn("--source-generations", display["command"])
        sources = [item for item in plan["processes"]
                   if item["name"].startswith("external-source-")]
        self.assertEqual(len(sources), 3)
        for item in sources:
            self.assertIn("--event-stream-dir", item["command"])
            self.assertIn("--source-epoch", item["command"])
            self.assertIn("--source-generations", item["command"])
            self.assertIn("--window-count", item["command"])
            self.assertNotIn("--video-source", item["command"])

    def test_video_reference_passport_binds_all_source_videos_and_model(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            for name in ("uav1_1min.mp4", "uav2_1min.mp4", "uav3_1min.mp4"):
                (source / name).write_bytes(name.encode())
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            with patch("Experiments.UAV.tracking_artifacts._run_ffprobe",
                       return_value={"width": 640, "height": 480}):
                passport = validate_video_reference_assets(
                    source, model, "local-test-accepted")

        self.assertEqual(passport["mode"], "video-reference-simulated")
        self.assertEqual([item["cameraId"] for item in passport["videos"]],
                         ["UAV1", "UAV2", "UAV3"])
        self.assertTrue(all(item["digest"].startswith("sha256:")
                            for item in passport["videos"]))
        self.assertTrue(passport["model"]["digest"].startswith("sha256:"))
        self.assertTrue(passport["candidateDigest"].startswith("sha256:"))

    def test_child_pythonpath_preserves_launcher_import_roots(self) -> None:
        paths = _child_pythonpath().split(":")
        self.assertIn(str(ROOT), paths)
        self.assertTrue(any(path.endswith(("site-packages", "dist-packages"))
                            for path in paths))

    def test_plan_has_exact_five_nodes_and_gs_local_controller(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            plan = build_plan(source=ASSETS, model=MODEL,
                              output=Path(temporary), license_text="local-test-accepted",
                              headless=True, legacy_mode=True)
        self.assertEqual(plan["topology"]["nodes"], ["compute", "gs", "uav1", "uav2", "uav3"])
        self.assertEqual({item["node"] for item in plan["processes"]},
                         {"compute", "gs", "uav1", "uav2", "uav3"})
        self.assertEqual([item for item in plan["processes"] if item["name"] == "controller"][0]["node"], "gs")
        self.assertNotIn("controller", plan["topology"]["nodes"])
        identities = {item["name"]: item["identity"] for item in plan["processes"]}
        self.assertEqual(identities["uav1-camera"], "/example/uav/drone/UAV1")
        self.assertEqual(identities["uav2-camera"], "/example/uav/drone/UAV2")
        self.assertEqual(identities["uav3-camera"], "/example/uav/drone/UAV3")
        self.assertEqual(plan["settings"]["windowCount"], 3)
        for camera in ("UAV1", "UAV2", "UAV3"):
            process = plan["processes"][[item["name"] for item in plan["processes"]].index(
                f"uav{camera[-1]}-camera")]
            self.assertIn("--frame-dir", process["command"])
        self.assertIn("--window-count", plan["processes"][-2]["command"])

    def test_plan_rejects_unbounded_or_empty_replay(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            for count in (0, 61):
                with self.assertRaises(RunnerError):
                    build_plan(source=ASSETS, model=MODEL,
                               output=Path(temporary), license_text="local-test-accepted",
                               headless=True, window_count=count, legacy_mode=True)

    def test_late_response_loss_is_an_explicit_bounded_fault(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            plan = build_plan(source=ASSETS, model=MODEL,
                              output=Path(temporary), license_text="local-test-accepted",
                              headless=True, window_count=3,
                              fault="late-response-loss")
        self.assertEqual(plan["settings"]["fault"], "late-response-loss")
        compute = next(item for item in plan["processes"] if item["name"] == "compute")
        self.assertIn("late-response-loss", compute["command"])

    def test_reference_digest_fault_targets_the_distributed_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "model.pt"
            model.write_bytes(b"model-checkpoint-placeholder")
            with patch(
                    "Experiments.UAV.run_multicamera_tracking_demo.validate_video_reference_assets",
                    return_value={"testAssets": True}):
                plan = build_plan(
                    source=root / "videos", model=model,
                    output=root / "run", license_text="local-test-accepted",
                    headless=True, window_count=3, fault="bad-reference-digest",
                    observation_mode="live-push")
        self.assertEqual(plan["settings"]["fault"], "bad-reference-digest")
        source = next(item for item in plan["processes"] if item["name"] == "uav3-camera")
        self.assertIn("bad-reference-digest", source["command"])
        pusher = next(item for item in plan["processes"]
                      if item["name"] == "observation-push-uav3")
        self.assertIn("--push-video", pusher["command"])

    def test_run_preflight_reports_missing_native_and_root_without_starting(self) -> None:
        if not (ASSETS / "uav1_1min.mp4").is_file():
            self.skipTest("real Spec191 assets are not installed on this host")
        with tempfile.TemporaryDirectory() as temporary:
            plan = build_plan(source=ASSETS, model=MODEL,
                              output=Path(temporary), license_text="local-test-accepted",
                              headless=True)
            with patch("Experiments.UAV.run_multicamera_tracking_demo.os.geteuid", return_value=1000):
                report = preflight(plan, run=True, headless=True)
        self.assertFalse(report["ok"])
        self.assertIn("MiniNDN run requires root privileges", report["errors"])

    def test_supervisor_rejects_missing_executable_before_child_start(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            supervisor = ProcessSupervisor(Path(temporary))
            from Experiments.UAV.run_multicamera_tracking_demo import ProcessSpec
            spec = ProcessSpec("missing", "gs", "/id", Path(temporary),
                               ("does-not-exist-spec191",), Path(temporary) / "private")
            with self.assertRaises(RunnerError):
                supervisor.start(spec, {"PATH": "/usr/bin"})


if __name__ == "__main__":
    unittest.main()
