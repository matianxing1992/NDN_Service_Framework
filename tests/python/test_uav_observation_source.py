from __future__ import annotations

import base64
from io import BytesIO
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))

from observation_source import (  # noqa: E402
    encode_event,
    load_demo_profile,
    load_observation_profile,
    parse_observation_profile,
    load_source_generation_manifest,
    profile_events,
    push_video,
    source_generation_for_window,
    write_event,
    write_profile_frame,
)


class ObservationSourceWriterTests(unittest.TestCase):
    @staticmethod
    def _gap() -> dict[str, object]:
        return {
            "schema": "uav-frame-gap-v2",
            "identity": {"sourceId": "source-1", "droneId": "drone-1", "cameraId": "UAV1",
                         "epoch": 7, "worldFrameId": "synthetic-local",
                         "acquisitionKind": "simulated", "deliveryMode": "live-push"},
            "firstSequence": 2, "lastSequence": 4, "reason": "CAPTURE_DROPPED",
        }

    def test_gap_event_records_only_declared_loss_with_canonical_integer_fields(self) -> None:
        gap = self._gap()
        line = encode_event("gap", gap)
        self.assertTrue(line.endswith(b"\n"))
        event = json.loads(line)
        self.assertEqual(event["schema"], "uav-source-event-v2")
        self.assertEqual(event["kind"], "gap")
        self.assertEqual(set(event["payload"]),
                         {"schema", "identity", "firstSequence", "lastSequence", "reason"})
        self.assertEqual(event["payload"]["firstSequence"], "2")
        self.assertEqual(event["payload"]["lastSequence"], "4")
        self.assertEqual(event["payload"]["identity"]["epoch"], "7")
        self.assertEqual(event["payload"]["reason"], "CAPTURE_DROPPED")
        stream = BytesIO()
        write_event(stream, "gap", gap)
        self.assertEqual(stream.getvalue(), line)
        for mode in ("generated", "replay", "live-push"):
            with self.subTest(mode=mode):
                gap["identity"] = {**gap["identity"], "deliveryMode": mode}
                self.assertEqual(json.loads(encode_event("gap", gap))["payload"]["identity"]["deliveryMode"], mode)
        gap["firstSequence"], gap["lastSequence"] = 1, 2**63 - 1
        self.assertEqual(json.loads(encode_event("gap", gap))["payload"]["lastSequence"],
                         "9223372036854775807")

    def test_gap_event_rejects_malformed_ranges_identity_and_invented_capture_fields(self) -> None:
        invalid = {
            "schema": ["uav-frame-gap-v99", [], None],
            "firstSequence": [0, -1, True, 1.5, "2", 5],
            "lastSequence": [1, 2**63, False, "4"],
            "reason": ["invented", [], None],
            "identity": [None, {}, {**self._gap()["identity"], "epoch": True},
                         {**self._gap()["identity"], "sourceId": ""},
                         {**self._gap()["identity"], "sourceId": "é" * 65},
                         {**self._gap()["identity"], "sourceId": "bad\0id"},
                         {**self._gap()["identity"], "deliveryMode": []},
                         {**self._gap()["identity"], "acquisitionKind": {}}],
        }
        for field, values in invalid.items():
            for value in values:
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    encode_event("gap", {**self._gap(), field: value})
        for extra in ("captureTimeUs", "deviceTimeUs", "imageBytes", "imageBase64"):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                encode_event("gap", {**self._gap(), extra: 0})

    def test_profile_rejects_invalid_numeric_types_and_source_fields(self) -> None:
        original = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text())
        invalid = {
            "schema": [[], {}, None],
            "acquisitionKind": [[], {}, None],
            "geometryValidity": [[], {}, None],
            "frameCount": [True, False, 1.5, 1.0, "100", None, 0, 601],
            "sampleRateHz": [True, False, "5", None, 0.5, 31,
                             float("nan"), float("inf"), -float("inf")],
            "source": [{"kind": "video-reference", "fd": 3},
                       {"kind": "external-fd", "eventPath": "events.jsonl"},
                       {"kind": []}, {"kind": {}}, {"kind": None}],
        }
        for schema in ("uav-observation-demo-profile-v1", "uav-observation-profile-v2"):
            for field, values in invalid.items():
                for value in values:
                    with self.subTest(schema=schema, field=field, value=value):
                        profile = {**original, "schema": schema, field: value}
                        with self.assertRaises(ValueError):
                            parse_observation_profile(json.dumps(profile).encode())

    def test_profile_accepts_numeric_bounds_and_both_source_kinds(self) -> None:
        original = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text())
        for count, rate in ((1, 1), (600, 30.0), (100, 2.5)):
            for kind in ("video-reference", "external-fd"):
                profile = {**original, "frameCount": count, "sampleRateHz": rate,
                           "source": {"kind": kind}}
                parsed = parse_observation_profile(json.dumps(profile).encode())
                self.assertEqual(parsed["frameCount"], count)
                self.assertEqual(parsed["sampleRateHz"], rate)
                self.assertEqual(parsed["source"], {"kind": kind})

    def test_profile_versions_normalize_without_changing_events_or_input_bytes(self) -> None:
        original = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text())
        profiles, events = [], []
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            for schema in ("uav-observation-demo-profile-v1", "uav-observation-profile-v2"):
                original["schema"] = schema
                raw = (json.dumps(original, indent=3) + "\n\n").encode()
                path.write_bytes(raw)
                profile = load_observation_profile(path)
                self.assertEqual(profile["schema"], "uav-observation-profile-v2")
                self.assertEqual(path.read_bytes(), raw)
                self.assertEqual(profile["acquisitionKind"], "simulated")
                self.assertEqual(profile["geometryValidity"], "illustrative")
                self.assertEqual(profile["accuracyQualification"], "unvalidated")
                profiles.append(profile)
                events.append(list(profile_events(profile, "UAV1", 1, 0, b"jpeg",
                                                  source_id="same-source")))
            self.assertEqual(profiles[0], profiles[1])
            self.assertEqual(events[0], events[1])
            original["schema"] = "uav-observation-profile-v99"
            path.write_text(json.dumps(original))
            with self.assertRaisesRegex(ValueError, "unsupported"):
                load_observation_profile(path)

    @staticmethod
    def _clock() -> dict[str, object]:
        return {"revision": 1, "clockId": "media-pts", "clockEpoch": 1, "a": 1.0,
                "bUs": 0.0, "validFromUs": 0, "validUntilUs": 2**63 - 1,
                "uncertaintyUs": 0}

    def test_writer_encodes_numeric_wire_leaves_as_strings(self) -> None:
        payload = {
            "schema": "uav-calibration-v2", "cameraId": "UAV1", "revision": 1,
            "width": 640, "height": 480, "fx": 500.0, "fy": 501.0,
            "cx": 320.0, "cy": 240.0, "distortion": [0, 0, 0, 0, 0],
            "imageTransform": "raw", "validFromUs": 0, "validUntilUs": 1000,
        }
        event = json.loads(encode_event("calibration", payload))
        self.assertEqual(event["schema"], "uav-source-event-v2")
        self.assertEqual(event["payload"]["revision"], "1")
        self.assertEqual(event["payload"]["fx"], "500")
        self.assertEqual(event["payload"]["distortion"], ["0", "0", "0", "0", "0"])

    def test_calibration_event_preserves_a_valid_external_data_ref(self) -> None:
        payload = {
            "schema": "uav-calibration-v2", "cameraId": "UAV1", "revision": 1,
            "width": 640, "height": 480, "fx": 500.0, "fy": 501.0,
            "cx": 320.0, "cy": 240.0, "distortion": [0, 0, 0, 0, 0],
            "imageTransform": "raw", "validFromUs": 0, "validUntilUs": 1000,
            "externalDataRef": {
                "exactName": "/example/calibration/UAV/CALIBRATION/UAV1/1/v=1",
                "contentSha256": "sha256:" + "a" * 64,
                "producerIdentity": "/example/calibration",
            },
        }

        event = json.loads(encode_event("calibration", payload))

        self.assertEqual(event["payload"]["externalDataRef"], payload["externalDataRef"])

        empty_component_ref = {
            **payload["externalDataRef"],
            "exactName": "/example/.../calibration/UAV/CALIBRATION/UAV1/1/v=1",
            "producerIdentity": "/example/.../calibration",
        }
        event = json.loads(encode_event("calibration", {**payload,
                                                          "externalDataRef": empty_component_ref}))
        self.assertEqual(event["payload"]["externalDataRef"], empty_component_ref)

        alternate_typed_ref = {
            **payload["externalDataRef"],
            "exactName": ("/example/seg=1/seq=2/off=3/t=4/v=0/"
                          "UAV/CALIBRATION/UAV1/1/v=1"),
            "producerIdentity": "/example/seg=1/seq=2/off=3/t=4/v=0",
        }
        event = json.loads(encode_event("calibration", {**payload,
                                                          "externalDataRef": alternate_typed_ref}))
        self.assertEqual(event["payload"]["externalDataRef"], alternate_typed_ref)

        digest_component_ref = {
            **payload["externalDataRef"],
            "exactName": ("/example/sha256digest=" + "a" * 64 +
                          "/UAV/CALIBRATION/UAV1/1/v=1"),
            "producerIdentity": "/example/sha256digest=" + "a" * 64,
        }
        event = json.loads(encode_event("calibration", {**payload,
                                                          "externalDataRef": digest_component_ref}))
        self.assertEqual(event["payload"]["externalDataRef"], digest_component_ref)

        numeric_type_ref = {
            **payload["externalDataRef"],
            "exactName": "/example/32=keyword/UAV/CALIBRATION/UAV1/1/v=1",
            "producerIdentity": "/example/32=keyword",
        }
        event = json.loads(encode_event("calibration", {**payload,
                                                          "externalDataRef": numeric_type_ref}))
        self.assertEqual(event["payload"]["externalDataRef"], numeric_type_ref)

    def test_profile_events_forward_external_calibration_reference_unchanged(self) -> None:
        profile = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text())
        reference = {
            "exactName": "/example/calibration/UAV/CALIBRATION/UAV1/1/v=1",
            "contentSha256": "sha256:" + "b" * 64,
            "producerIdentity": "/example/calibration",
        }
        profile["cameras"]["UAV1"]["calibration"]["externalDataRef"] = reference
        profile = parse_observation_profile(json.dumps(profile).encode())

        events = dict(profile_events(profile, "UAV1", 1, 0, b"jpeg", source_id="source"))

        self.assertEqual(events["calibration"]["externalDataRef"], reference)
        self.assertEqual(json.loads(encode_event("calibration", events["calibration"]))["payload"]["externalDataRef"],
                         reference)

    def test_calibration_event_rejects_malformed_external_data_ref(self) -> None:
        payload = {
            "schema": "uav-calibration-v2", "cameraId": "UAV1", "revision": 1,
            "width": 640, "height": 480, "fx": 500.0, "fy": 501.0,
            "cx": 320.0, "cy": 240.0, "distortion": [0, 0, 0, 0, 0],
            "imageTransform": "raw", "validFromUs": 0, "validUntilUs": 1000,
        }
        valid_reference = {
            "exactName": "/example/calibration/UAV/CALIBRATION/UAV1/1/v=1",
            "contentSha256": "sha256:" + "c" * 64,
            "producerIdentity": "/example/calibration",
        }
        invalid = [
            None,
            {},
            {**valid_reference, "contentSha256": "sha256:ABC"},
            {**valid_reference, "producerIdentity": ""},
            {**valid_reference, "producerIdentity": "/",
             "exactName": "/UAV/CALIBRATION/UAV1/1/v=1"},
            {**valid_reference, "producerIdentity": "/example/seg=01",
             "exactName": "/example/seg=01/UAV/CALIBRATION/UAV1/1/v=1"},
            {**valid_reference,
             "producerIdentity": "/example/sha256digest=" + "A" * 64,
             "exactName": "/example/sha256digest=" + "A" * 64 +
                          "/UAV/CALIBRATION/UAV1/1/v=1"},
            {**valid_reference, "producerIdentity": "/example/50=.......",
             "exactName": "/example/50=......./UAV/CALIBRATION/UAV1/1/v=1"},
            {**valid_reference,
             "exactName": "/example/calibration/UAV/CALIBRATION/UAV1/1/v=18446744073709551616"},
            {**valid_reference, "unexpected": "field"},
            {**valid_reference,
             "exactName": "/example/calibration/UAV/CALIBRATION/UAV2/1/v=1"},
            {**valid_reference,
             "exactName": "/example/calibration/UAV/GROUND/UAV1/1/v=1"},
            {**valid_reference,
             "exactName": "/example/calibration/UAV/CALIBRATION/UAV1/2/v=1"},
            {**valid_reference,
             "exactName": "/example/calibration/UAV/CALIBRATION/UAV1/%31/v=1"},
            {**valid_reference,
             "producerIdentity": "/example/cal%69bration",
             "exactName": "/example/cal%69bration/UAV/CALIBRATION/UAV1/1/v=1"},
        ]

        for reference in invalid:
            with self.subTest(reference=reference), self.assertRaises(ValueError):
                encode_event("calibration", {**payload, "externalDataRef": reference})

        profile = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json").read_text())
        profile["cameras"]["UAV1"]["calibration"]["externalDataRef"] = None
        with self.assertRaises(ValueError):
            parse_observation_profile(json.dumps(profile).encode())

    def test_frame_writer_transfers_owned_bytes_and_flushes(self) -> None:
        payload = {
            "key": {"sourceId": "src", "cameraId": "UAV1", "epoch": 4, "sequence": 9},
            "deviceTimeUs": 123456, "clockBinding": self._clock(), "width": 640, "height": 480,
            "encoding": "image/jpeg", "calibrationRevision": 1, "imageBytes": b"\xff\xd8frame\xff\xd9",
        }
        stream = BytesIO()
        write_event(stream, "frame", payload)
        self.assertTrue(stream.getvalue().endswith(b"\n"))
        event = json.loads(stream.getvalue())
        self.assertEqual(event["payload"]["deviceTimeUs"], "123456")
        self.assertEqual(base64.b64decode(event["payload"]["imageBase64"], validate=True),
                         b"\xff\xd8frame\xff\xd9")

    def test_writer_rejects_wrong_numeric_types_and_non_finite_values(self) -> None:
        for value in (True, None, math.inf, math.nan, -1):
            with self.subTest(value=value):
                payload = {
                    "schema": "uav-calibration-v2", "cameraId": "UAV1", "revision": value,
                    "width": 640, "height": 480, "fx": 500.0, "fy": 500.0,
                    "cx": 320.0, "cy": 240.0, "distortion": [0, 0, 0, 0, 0],
                    "imageTransform": "raw", "validFromUs": 0, "validUntilUs": 1000,
                }
                with self.assertRaises(ValueError):
                    encode_event("calibration", payload)

    def test_writer_rejects_unknown_fields_and_noncanonical_image_base64(self) -> None:
        calibration = {
            "schema": "uav-calibration-v2", "cameraId": "UAV1", "revision": 1,
            "width": 640, "height": 480, "fx": 500.0, "fy": 500.0,
            "cx": 320.0, "cy": 240.0, "distortion": [0, 0, 0, 0, 0],
            "imageTransform": "raw", "validFromUs": 0, "validUntilUs": 1000,
            "unexpected": "not-allowed",
        }
        with self.assertRaisesRegex(ValueError, "unknown fields"):
            encode_event("calibration", calibration)

        frame = {
            "key": {"sourceId": "src", "cameraId": "UAV1", "epoch": 4, "sequence": 1},
            "deviceTimeUs": 0, "clockBinding": self._clock(), "width": 640, "height": 480,
            "encoding": "image/jpeg", "calibrationRevision": 1, "imageBase64": "/9j/2T==",
        }
        with self.assertRaisesRegex(ValueError, "canonical"):
            encode_event("frame", frame)

    def test_demo_profile_is_simulated_and_yields_one_common_source_path(self) -> None:
        profile = load_demo_profile(ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
        self.assertEqual(profile["geometryValidity"], "illustrative")
        self.assertEqual(profile["accuracyQualification"], "unvalidated")
        events = list(profile_events(profile, "UAV2", 17, 3_400_000, b"\xff\xd8x\xff\xd9",
                                     source_id="demo-run-UAV2"))
        self.assertEqual([kind for kind, _ in events], ["calibration", "ground", "pose", "frame"])
        pose = events[2][1]
        self.assertEqual(pose["identity"]["acquisitionKind"], "simulated")
        self.assertEqual(pose["sampleSequence"], 17)
        self.assertEqual(events[0][1]["width"], profile["cameras"]["UAV2"]["width"])
        lines = b"".join(encode_event(kind, payload) for kind, payload in events)
        self.assertEqual(len(lines.splitlines()), 4)

    def test_profile_geometry_uses_contract_values(self) -> None:
        profile = json.loads((ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
                             .read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            profile["geometryValidity"] = "calibrated-plane"
            path.write_text(json.dumps(profile), encoding="utf-8")
            self.assertEqual(load_observation_profile(path)["geometryValidity"],
                             "calibrated-plane")
            profile["geometryValidity"] = "valid"
            path.write_text(json.dumps(profile), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "geometryValidity"):
                load_observation_profile(path)

    def test_replay_and_live_push_bind_identical_capture_facts_and_image_bytes(self) -> None:
        base = load_demo_profile(ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
        image = b"\xff\xd8one-identical-frame\xff\xd9"
        outputs = {}
        for mode in ("replay", "live-push"):
            profile = dict(base)
            profile["deliveryMode"] = mode
            stream = BytesIO()
            write_profile_frame(stream, profile, "UAV1", 9, 1_800_000, image,
                                source_id="same-source", epoch=11,
                                image_width=1920, image_height=1080)
            events = [json.loads(line) for line in stream.getvalue().splitlines()]
            for event in events:
                identity = event["payload"].get("identity")
                if identity is not None:
                    self.assertEqual(identity["deliveryMode"], mode)
                    del identity["deliveryMode"]
            outputs[mode] = events
        self.assertEqual(outputs["replay"], outputs["live-push"])
        frame = outputs["replay"][-1]["payload"]
        self.assertEqual(base64.b64decode(frame["imageBase64"], validate=True), image)
        self.assertEqual(frame["deviceTimeUs"], "1800000")

    def test_generation_manifest_changes_all_camera_ids_at_a_sequential_epoch_boundary(self) -> None:
        manifest = {
            "schema": "uav-source-generations-v1",
            "windowCount": 4,
            "generations": [
                {"firstWindowIndex": 0, "sourceEpoch": 7,
                 "sourceIds": {camera: f"first-{camera}" for camera in ("UAV1", "UAV2", "UAV3")}},
                {"firstWindowIndex": 2, "sourceEpoch": 8,
                 "sourceIds": {camera: f"second-{camera}" for camera in ("UAV1", "UAV2", "UAV3")}},
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source-generations.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            loaded = load_source_generation_manifest(path, 4)
            self.assertEqual(source_generation_for_window(loaded, 1)["sourceEpoch"], 7)
            self.assertEqual(source_generation_for_window(loaded, 2)["sourceIds"]["UAV3"],
                             "second-UAV3")
            with self.assertRaisesRegex(ValueError, "windowCount"):
                load_source_generation_manifest(path, 3)
            malformed = dict(manifest)
            malformed["generations"] = [dict(item) for item in manifest["generations"]]
            malformed["generations"][1] = dict(malformed["generations"][1])
            malformed["generations"][1]["sourceEpoch"] = 9
            path.write_text(json.dumps(malformed), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "advance by one"):
                load_source_generation_manifest(path, 4)

    def test_live_reference_push_decodes_on_demand_and_uses_source_pts(self) -> None:
        class Capture:
            def __init__(self) -> None:
                self.index = 0

            def isOpened(self) -> bool:
                return True

            def read(self):
                if self.index >= 4:
                    return False, None
                self.index += 1
                return True, np.zeros((24, 32, 3), dtype=np.uint8)

            def release(self) -> None:
                pass

        pts = [1000, 101000, 201000, 301000]
        probe = SimpleNamespace(
            returncode=0, stderr="",
            stdout=json.dumps({"frames": [
                {"best_effort_timestamp_time": f"{value / 1_000_000:.6f}"}
                for value in pts
            ]}))
        capture = Capture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile_path = ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json"
            video = root / "reference.mp4"
            video.write_bytes(b"runtime-reference-placeholder")
            fifo_dir = root / "pipes"
            fifo_dir.mkdir()
            generation_path = root / "source-generations.json"
            generation_path.write_text(json.dumps({
                "schema": "uav-source-generations-v1", "windowCount": 2,
                "generations": [
                    {"firstWindowIndex": 0, "sourceEpoch": 8,
                     "sourceIds": {camera: f"first-{camera}" for camera in ("UAV1", "UAV2", "UAV3")}},
                    {"firstWindowIndex": 1, "sourceEpoch": 9,
                     "sourceIds": {camera: f"second-{camera}" for camera in ("UAV1", "UAV2", "UAV3")}},
                ],
            }), encoding="utf-8")
            with patch("observation_source.subprocess.run", return_value=probe) as run_probe, \
                    patch("cv2.VideoCapture", return_value=capture), \
                    patch("builtins.print") as ready_output:
                push_video(video, profile_path, fifo_dir, camera="UAV1",
                           source_id="live-reference-UAV1", epoch=8,
                           window_count=2, sample_rate_hz=5.0,
                           source_generations_path=generation_path)

            ready_output.assert_any_call(
                "UAV_OBSERVATION_PUSHER_READY camera=UAV1 windows=2 "
                f"generation-manifest-sha256=sha256:{hashlib.sha256(generation_path.read_bytes()).hexdigest()}",
                flush=True)

            command = run_probe.call_args.args[0]
            interval = command.index("-read_intervals")
            self.assertEqual(command[interval + 1], "%+1.200000")

            first = [json.loads(line) for line in
                     (fifo_dir / "window-0.jsonl").read_text().splitlines()]
            second = [json.loads(line) for line in
                      (fifo_dir / "window-1.jsonl").read_text().splitlines()]
        self.assertEqual([item["kind"] for item in first], ["calibration", "ground", "pose", "frame"])
        self.assertEqual(first[2]["payload"]["identity"]["deliveryMode"], "live-push")
        self.assertEqual(first[3]["payload"]["key"]["sourceId"], "first-UAV1")
        self.assertEqual(first[3]["payload"]["deviceTimeUs"], "0")
        self.assertEqual(second[3]["payload"]["deviceTimeUs"], "200000")
        self.assertEqual(second[2]["payload"]["identity"]["sourceId"], "second-UAV1")
        self.assertEqual(second[3]["payload"]["key"]["sourceId"], "second-UAV1")
        self.assertEqual(second[3]["payload"]["key"]["epoch"], "9")
        self.assertEqual(second[3]["payload"]["width"], "32")


if __name__ == "__main__":
    unittest.main()

