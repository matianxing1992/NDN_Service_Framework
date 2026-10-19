from __future__ import annotations

import base64
from dataclasses import replace
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
from tracking_display import (ComputeRenderer, DisplayFrame, DisplayMailbox, _fit_text, _track_color,
                              _track_metric_label, _track_status_label, DISPLAY_WINDOW_SIZE,
                              METRICS_PANEL_HEIGHT, METRICS_ROW_HEIGHT, METRICS_PAGE_SECONDS,
                              _matches_observation_scope,
                              _legacy_camera_speeds, serve)  # noqa: E402
from display_clock import ClockSample  # noqa: E402
from observation_source import load_observation_profile  # noqa: E402


class TrackingDisplayTest(unittest.TestCase):
    def test_legacy_duplicate_camera_track_estimates_stay_unknown(self) -> None:
        estimates = [{"cameraId": "UAV1", "globalId": 7, "vehicleSpeedMmps": speed, "status": "ok"}
                     for speed in (5000, 9000)]
        estimates += [{"cameraId": "UAV2", "globalId": 7, "vehicleSpeedMmps": 12000, "status": "ok"},
                      {"globalId": 7, "vehicleSpeedMmps": 99000, "status": "ok"}]
        self.assertEqual(_legacy_camera_speeds(estimates), {("UAV2", 7): 12000})

    def test_local_source_clock_api_ages_without_a_new_frame(self) -> None:
        from test_uav_display_clock import _timing
        raw, clock = _timing(measured=True)
        now = [100]
        renderer = ComputeRenderer("headless", steady_now_us=lambda: now[0])
        renderer.start()
        frame = replace(self.frame("UAV1", 0), source_identity=raw["identity"],
                        clock_binding=raw["clockBinding"], time_uncertainty_us=0, age_clock=clock)
        self.assertEqual(DisplayFrame.from_dict(frame.to_dict()), frame)
        renderer.submit(frame)
        with patch.object(renderer, "_emit_frame_shown") as events:
            renderer.pump_once()
            self.assertEqual(renderer.status["captureAge"]["UAV1"]["freshness"], "fresh")
            now[0] = 1_200_100
            renderer.pump_once()
            self.assertEqual(renderer.status["captureAge"]["UAV1"]["freshness"], "stale")
            self.assertEqual(renderer.status["captureAge"]["UAV1"]["basis"], "source")
            self.assertEqual(events.call_count, 1)

    def test_legacy_result_reader_shows_only_bound_unqualified_estimates(self) -> None:
        shown, pumps = [], [0]
        original_pump = ComputeRenderer.pump_once

        def pump(renderer, *args):
            result = original_pump(renderer, *args)
            pumps[0] += 1
            return result and not shown and pumps[0] < 20

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            frames = []
            for number in (1, 2, 3):
                camera = "UAV" + str(number)
                (root / (camera + ".jpg")).write_bytes(b"jpeg")
                frames.append({"cameraId": camera, "sequence": 1, "ptsUs": 0,
                               "imagePath": camera + ".jpg",
                               "tracks": [{"localId": number, "globalId": 7}]})
            path = root / "result.json"
            path.write_text(json.dumps({
                "schema": "spec191-tracking-result-v1", "runId": "r", "missionId": "m",
                "windowId": "window-0", "frames": frames, "motion": {
                    "schema": "spec191-motion-result-v1", "inputProvenance": "simulated-input",
                    "estimates": [{"cameraId": "UAV1", "globalId": 7,
                                   "vehicleSpeedMmps": 5000, "status": "ok"},
                                  {"cameraId": "UAV2", "globalId": 7,
                                   "vehicleSpeedMmps": 12000, "status": "ok"},
                                  {"globalId": 7, "vehicleSpeedMmps": 99000, "status": "ok"},
                                  {"cameraId": "UAV3", "globalId": 7,
                                   "vehicleSpeedMmps": 99000, "status": "degraded"}]}}))
            with patch.object(ComputeRenderer, "pump_once", pump), \
                 patch.object(ComputeRenderer, "_emit_frame_shown", side_effect=lambda frame: shown.append(frame)):
                self.assertEqual(serve(root / "display.sock", "headless", 0, path, root), 0)
        self.assertEqual(len(shown), 3)
        self.assertEqual(_track_metric_label(shown[0].tracks[0]),
                         "L1/G7  18.00km/h")
        self.assertEqual(_track_metric_label(shown[1].tracks[0]),
                         "L2/G7  43.20km/h")
        self.assertEqual(_track_metric_label(shown[2].tracks[0]),
                         "L3/G7  ?km/h")
        self.assertTrue(all(_track_status_label(frame.tracks[0]) == "legacy estimate / unqualified"
                            for frame in shown))
        self.assertEqual(shown[0].speed_mmps, (5000,))
        self.assertEqual(shown[1].speed_mmps, (12000,))
        self.assertEqual(shown[2].speed_mmps, ())
        self.assertTrue(all(frame.source_identity is None and frame.accuracy_qualification == "unvalidated"
                            for frame in shown))

    def test_provider_failure_bad_values_and_recovery_keep_cached_frame(self) -> None:
        from test_uav_display_clock import _timing
        raw, clock = _timing()
        valid = {"UAV1": ClockSample.parse(clock)}
        values = [valid, OSError("disconnected"), RuntimeError("device unavailable"), ValueError("bad clock"),
                  TypeError("invalid sample"), None, {"UAV1": {}}, valid]

        def provider():
            value = values.pop(0)
            if isinstance(value, Exception):
                raise value
            return value

        renderer = ComputeRenderer("headless", clock_provider=provider, steady_now_us=lambda: 100)
        renderer.start()
        renderer.submit(replace(self.frame("UAV1", 0), source_identity=raw["identity"],
                                clock_binding=raw["clockBinding"], time_uncertainty_us=0, age_clock=clock))
        with patch.object(renderer, "_emit_frame_shown") as events:
            for expected in ["fresh"] + ["unknown"] * 6 + ["fresh"]:
                renderer.pump_once()
                self.assertEqual(renderer.status["captureAge"]["UAV1"]["freshness"], expected)
            self.assertEqual(events.call_count, 1)
        self.assertEqual(renderer.status["shownFrames"], 1)

    def test_observation_scope_checks_all_identity_fields_and_scheduled_cutover(self) -> None:
        profile = load_observation_profile(ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
        profile["frameCount"] = 2
        sources = {camera: "source-" + camera for camera in ("UAV1", "UAV2", "UAV3")}
        generations = {"windowCount": 2, "generations": [
            {"firstWindowIndex": 0, "sourceEpoch": 7, "sourceIds": sources},
            {"firstWindowIndex": 1, "sourceEpoch": 8,
             "sourceIds": {camera: value + "-new" for camera, value in sources.items()}},
        ]}
        identity = {"sourceId": "source-UAV1", "droneId": profile["cameras"]["UAV1"]["droneId"],
                    "cameraId": "UAV1", "epoch": 7, "worldFrameId": profile["worldFrameId"],
                    "acquisitionKind": profile["acquisitionKind"], "deliveryMode": profile["deliveryMode"]}
        self.assertTrue(_matches_observation_scope("UAV1", 0, 1, identity, profile, generations))
        for field, value in (("sourceId", "other"), ("droneId", "other"), ("cameraId", "UAV2"),
                             ("epoch", 6), ("worldFrameId", "other"),
                             ("acquisitionKind", "measured"), ("deliveryMode", "live-push")):
            with self.subTest(field=field):
                changed = dict(identity, **{field: value})
                self.assertFalse(_matches_observation_scope("UAV1", 0, 1, changed, profile, generations))
        self.assertFalse(_matches_observation_scope("UAV1", 0, 2, identity, profile, generations))
        self.assertFalse(_matches_observation_scope("UAV1", 1, 2, identity, profile, generations))
        changed = dict(identity, epoch=8, sourceId="source-UAV1-new")
        self.assertTrue(_matches_observation_scope("UAV1", 1, 2, changed, profile, generations))
        self.assertFalse(_matches_observation_scope("UAV1", 2, 3, changed, profile, generations))

    def test_reused_output_and_socket_ignore_old_run_then_accept_current_frames(self) -> None:
        profile = load_observation_profile(ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json")
        profile["frameCount"] = 2
        sources = {camera: "source-" + camera for camera in ("UAV1", "UAV2", "UAV3")}
        generations = {"schema": "uav-source-generations-v1", "windowCount": 2, "generations": [
            {"firstWindowIndex": 0, "sourceEpoch": 8, "sourceIds": sources}]}
        shown, errors, pumps = [], [], [0]
        halt = threading.Event()
        original_pump = ComputeRenderer.pump_once

        def pump(renderer, *args):
            value = original_pump(renderer, *args)
            pumps[0] += 1
            return value and not halt.is_set()

        def wait_until(predicate):
            deadline = time.monotonic() + 3
            while not predicate():
                if time.monotonic() >= deadline:
                    self.fail("renderer did not reach the expected bounded state")
                halt.wait(0.01)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile_path, generation_path = root / "profile.json", root / "generations.json"
            profile_path.write_text(json.dumps(profile))
            generation_path.write_text(json.dumps(generations))
            result_path, socket_path = root / "tracking-result.json", root / "display.sock"
            frames = []
            for camera in sources:
                (root / (camera + ".jpg")).write_bytes(b"jpeg")
                frames.append({"cameraId": camera, "sequence": 1, "ptsUs": 0,
                               "imagePath": camera + ".jpg", "tracks": [],
                               "observation": {"identity": {
                                   "sourceId": sources[camera], "cameraId": camera, "epoch": 7,
                                   "droneId": profile["cameras"][camera]["droneId"],
                                   "worldFrameId": profile["worldFrameId"],
                                   "acquisitionKind": profile["acquisitionKind"],
                                   "deliveryMode": profile["deliveryMode"]}}})
            result = {"schema": "spec210-tracking-result-v2", "runId": "same-output",
                      "missionId": "m", "windowId": "window-0", "frames": frames}

            def publish():
                temporary = result_path.with_suffix(".tmp")
                temporary.write_text(json.dumps(result))
                temporary.replace(result_path)

            publish()

            def run():
                try:
                    serve(socket_path, "headless", 0, result_path, root,
                          observation_profile=profile_path, source_generations=generation_path)
                except Exception as error:
                    errors.append(error)

            with patch.object(ComputeRenderer, "pump_once", pump), \
                 patch.object(ComputeRenderer, "_emit_frame_shown", side_effect=lambda frame: shown.append(frame)):
                thread = threading.Thread(target=run, daemon=True)
                thread.start()
                client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    wait_until(lambda: pumps[0] >= 3 or errors)
                    self.assertEqual(errors, [])
                    self.assertEqual(shown, [])
                    for frame in frames:
                        frame["observation"]["identity"]["epoch"] = 8
                    publish()
                    wait_until(lambda: len(shown) == 3 or errors)
                    self.assertEqual(errors, [])
                    self.assertEqual(len(shown), 3)
                    client.connect(str(socket_path))
                    old = replace(self.frame("UAV1", 0), sequence=1, source_identity=
                                  dict(frames[0]["observation"]["identity"], epoch=7))
                    client.sendall(json.dumps(old.to_dict()).encode() + b"\n")
                    before = pumps[0]
                    wait_until(lambda: pumps[0] >= before + 3 or errors)
                    self.assertEqual(len(shown), 3)
                    current = replace(old, window=1, sequence=2, pts_us=200_000,
                                      source_identity=frames[0]["observation"]["identity"])
                    client.sendall(json.dumps(current.to_dict()).encode() + b"\n")
                    wait_until(lambda: len(shown) == 4 or errors)
                    self.assertEqual(errors, [])
                    self.assertEqual(shown[-1].sequence, 2)
                    client.sendall(b'{"event":"STOP"}\n')
                finally:
                    halt.set()
                    client.close()
                    thread.join(3)
                self.assertFalse(thread.is_alive())
                self.assertEqual(errors, [])

    def test_idle_cached_frame_repaints_age_without_decoding_or_counting_again(self) -> None:
        import cv2
        import numpy as np
        from test_uav_display_clock import _timing

        raw, clock = _timing()
        samples = {"UAV1": ClockSample.parse(clock)}
        ok, jpeg = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
        self.assertTrue(ok)
        frame = replace(self.frame("UAV1", 0), image_jpeg=bytes(jpeg), tracks=(),
                        source_identity=raw["identity"], clock_binding=raw["clockBinding"],
                        time_uncertainty_us=0, geometry_validity="illustrative",
                        provenance="simulated/replay", anchor_approximation=True,
                        observation_ref={"name": "/example/observation", "digest": "sha256:" + "a" * 64})
        self.assertEqual(DisplayFrame.from_dict(frame.to_dict()), frame)
        renderer = ComputeRenderer("windows", clock_provider=lambda: samples,
                                   steady_now_us=lambda: 100)
        renderer.status["ready"] = True
        renderer.submit(frame)
        labels = []
        with patch.object(cv2, "imshow") as shown, \
             patch.object(cv2, "waitKey", return_value=0), \
             patch.object(cv2, "imdecode", wraps=cv2.imdecode) as decode, \
             patch.object(cv2, "putText", side_effect=lambda image, label, *_: labels.append(label)), \
             patch.object(renderer, "_emit_frame_shown") as events:
            renderer.pump_once()
            self.assertEqual(renderer.status["captureAge"]["UAV1"]["freshness"], "fresh")
            clock["currentTimeUs"] = 1_200_000
            samples["UAV1"] = ClockSample.parse(clock)
            renderer.pump_once()
            self.assertEqual(shown.call_count, 2)
            self.assertEqual(decode.call_count, 1)
            self.assertEqual(events.call_count, 1)
        self.assertEqual(renderer.status["shownFrames"], 1)
        self.assertIn("playback age=1200ms stale", labels)
        self.assertTrue(any("illustrative" in label for label in labels))
        self.assertTrue(any("approximate ground contact" in label for label in labels))

    def test_connected_idle_socket_keeps_pumping_and_stops_cleanly(self) -> None:
        pumped = threading.Event()
        halt = threading.Event()
        calls, errors = [], []
        target = [1000]

        def pump(renderer, *_):
            calls.append(1)
            if len(calls) >= target[0]:
                pumped.set()
            return not halt.is_set()

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "display.sock"

            def run():
                try:
                    serve(path, "headless", 0)
                except Exception as error:
                    errors.append(error)

            with patch.object(ComputeRenderer, "pump_once", pump):
                thread = threading.Thread(target=run, daemon=True)
                thread.start()
                client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    deadline = time.monotonic() + 3
                    while True:
                        try:
                            client.connect(str(path))
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            if time.monotonic() >= deadline:
                                raise
                            halt.wait(0.01)
                    target[0] = len(calls) + 3
                    self.assertTrue(pumped.wait(2), "connected idle peer blocked renderer pump")
                    messages = [json.dumps(self.frame(camera, 1).to_dict()).encode()
                                for camera in ("UAV1", "UAV2")]
                    with patch("tracking_display.MAX_MESSAGE_BYTES", max(map(len, messages)) + 1):
                        pumped.clear()
                        target[0] = len(calls) + 3
                        client.sendall(b"\n".join(messages) + b"\n")
                        self.assertTrue(pumped.wait(2), "valid batched messages exceeded a per-line bound")
                    client.sendall(b'{"event":"STOP"}\n')
                finally:
                    halt.set()
                    client.close()
                    thread.join(3)
                self.assertFalse(thread.is_alive())
                self.assertEqual(errors, [])
                self.assertFalse(path.exists())

    def test_window_shown_event_requires_completed_gui_refresh(self) -> None:
        import cv2
        import numpy as np

        ok, jpeg = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
        self.assertTrue(ok)
        for refresh_fails in (False, True):
            with self.subTest(refresh_fails=refresh_fails):
                renderer = ComputeRenderer(mode="windows")
                renderer.status["ready"] = True
                renderer.submit(replace(self.frame("UAV1", 1), image_jpeg=bytes(jpeg), tracks=()))
                events = []

                def refresh(_wait_ms):
                    self.assertEqual(renderer.status["shownFrames"], 0)
                    events.append("refresh")
                    if refresh_fails:
                        raise RuntimeError("GUI refresh failed")
                    return 0

                with patch.object(cv2, "imshow", side_effect=lambda *_: events.append("draw")), \
                     patch.object(cv2, "waitKey", side_effect=refresh), \
                     patch.object(renderer, "_emit_frame_shown", side_effect=lambda _: events.append("shown")):
                    if refresh_fails:
                        with self.assertRaisesRegex(RuntimeError, "GUI refresh failed"):
                            renderer.pump_once()
                    else:
                        self.assertTrue(renderer.pump_once())
                self.assertEqual(events, ["draw", "refresh"] + ([] if refresh_fails else ["shown"]))
                self.assertEqual(renderer.status["shownFrames"], 0 if refresh_fails else 1)

    def frame(self, camera: str, pts: int) -> DisplayFrame:
        return DisplayFrame("run", "session", 1, 0, camera, pts, pts,
                            b"jpeg", ({"box": [1, 2, 10, 11], "class": "car",
                                       "confidence": .9, "localId": 1, "globalId": 7},))

    def test_three_camera_slots_are_bounded_and_count_presentation_drops(self) -> None:
        mailbox = DisplayMailbox()
        for camera in ("UAV1", "UAV2", "UAV3"):
            mailbox.put(self.frame(camera, 1))
            mailbox.put(self.frame(camera, 2))
        self.assertEqual(mailbox.presentation_drops, {"UAV1": 1, "UAV2": 1, "UAV3": 1})
        self.assertEqual(mailbox.get_latest("UAV1").pts_us, 2)

    def test_rejects_unknown_camera_and_backwards_pts(self) -> None:
        mailbox = DisplayMailbox()
        with self.assertRaises(ValueError):
            mailbox.put(self.frame("other", 1))
        mailbox.put(self.frame("UAV1", 2))
        with self.assertRaises(ValueError):
            mailbox.put(self.frame("UAV1", 1))

    def test_metric_row_keeps_identity_speed_course_and_range_together(self) -> None:
        track = {
            "localId": 4,
            "globalId": 23,
            "measurement": {
                "schema": "uav-motion-result-v2",
                "speedMps": 12.345,
                "courseDeg": 270.0,
                "horizontalRangeM": 14.58,
                "slantRangeM": 15.24,
                "quality": "valid",
            },
        }
        self.assertEqual(
            _track_metric_label(track),
            "L4/G23  44.44km/h  270.0degT  H14.6/S15.2m")
        self.assertNotEqual(_track_color(track), _track_color({"localId": 5}))

    def test_unavailable_measurements_stay_unknown_in_metric_row(self) -> None:
        track = {
            "localId": 8,
            "globalId": 31,
            "measurement": {
                "schema": "uav-motion-result-v2",
                "speedMps": None,
                "courseDeg": None,
                "horizontalRangeM": None,
                "slantRangeM": None,
                "quality": "warming-up",
            },
        }
        label = _track_metric_label(track)
        self.assertEqual(label, "L8/G31  ?km/h  ?degT  H?/S?m")

    def test_two_line_panel_preserves_quality_reasons_and_numeric_values(self) -> None:
        import cv2
        import numpy as np

        # Expected labels are literal requirements, independently of the helpers.
        states = [
            ({"quality": "estimated", "reasonCodes": []}, "estimated"),
            ({"quality": "warming-up", "reasonCodes": ["INSUFFICIENT_TRACK_HISTORY"]},
             "warming-up INSUFFICIENT_TRACK_HISTORY"),
            ({"quality": "stationary", "reasonCodes": ["STATIONARY"]}, "stationary STATIONARY"),
            ({"quality": "degraded", "reasonCodes": ["POSE_UNAVAILABLE"]}, "degraded POSE_UNAVAILABLE"),
            ({"quality": "unknown", "reasonCodes": ["CALIBRATION_MISSING"]}, "unknown CALIBRATION_MISSING"),
            ({"reasonCodes": ["POSE_UNAVAILABLE"]}, "unknown POSE_UNAVAILABLE"),
            (None, "legacy estimate / unqualified"),
            ({"quality": "degraded", "reasonCodes": ["POSE_UNAVAILABLE", "TRACK_WARMUP"]},
             "degraded POSE_UNAVAILABLE (+1)"),
            ({"quality": "degraded", "reasonCodes": ["LOW_SPEED_DIRECTION_UNKNOWN"]},
             "degraded LOW_SPEED_DIRECTION_UNKNOWN"),
            ({"quality": "unknown", "reasonCodes": ["GROUND_INTERSECTION_BEHIND_CAMERA"]},
             "unknown GROUND_INTERSECTION_BEHIND_CAMERA"),
            ({"quality": "unknown", "reasonCodes": ["GROUND_INTERSECTION_OUTSIDE_ROI"]},
             "unknown GROUND_INTERSECTION_OUTSIDE_ROI"),
        ]
        tracks = []
        for number, (state, _) in enumerate(states, 1):
            track = {"localId": number, "globalId": number}
            if state is not None:
                track["measurement"] = {"schema": "uav-motion-result-v2", "speedMps": 12.345,
                                        "courseDeg": 270.0, "horizontalRangeM": 14.58,
                                        "slantRangeM": 15.24, **state}
            tracks.append(track)
        ok, jpeg = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
        self.assertTrue(ok)
        renderer = ComputeRenderer("windows")
        renderer.status["ready"] = True
        renderer.submit(replace(self.frame("UAV1", 0), image_jpeg=bytes(jpeg), tracks=tuple(tracks)))
        draws = []
        with patch.object(cv2, "imshow") as shown, \
             patch.object(cv2, "waitKey", return_value=0), \
             patch.object(cv2, "putText", side_effect=lambda image, label, xy, *args: draws.append((label, xy))), \
             patch("tracking_display.time.monotonic", return_value=0):
            renderer.pump_once()
        self.assertEqual(shown.call_args.args[1].shape[:2], DISPLAY_WINDOW_SIZE[::-1])
        for _, expected in states:
            self.assertIn(expected, [label for label, _ in draws])
            self.assertLessEqual(cv2.getTextSize(expected, cv2.FONT_HERSHEY_SIMPLEX, .33, 1)[0][0],
                                 DISPLAY_WINDOW_SIZE[0] // 2 - 30)
        self.assertIn("L1/G1  44.44km/h  270.0degT  H14.6/S15.2m", [label for label, _ in draws])
        first = next(xy for label, xy in draws if label == "estimated")
        numeric = next(xy for label, xy in draws if label.startswith("L1/G1  "))
        self.assertEqual(numeric, (first[0], first[1] + 17))
        # Eighth row starts the second column at the existing window dimensions.
        second_column = next(xy for label, xy in draws if label.endswith("(+1)"))
        self.assertEqual(second_column[0] - first[0], DISPLAY_WINDOW_SIZE[0] // 2)

    def test_two_line_panel_pages_cached_tracks_without_new_shown_or_decode(self) -> None:
        import cv2
        import numpy as np

        panel_height = min(METRICS_PANEL_HEIGHT, max(120, DISPLAY_WINDOW_SIZE[1] // 2))
        capacity = max(1, (panel_height - 50) // METRICS_ROW_HEIGHT) * 2
        self.assertEqual(capacity, 14)
        # Same-width integer IDs make the existing string sort order unambiguous.
        tracks = tuple({"localId": n, "globalId": n + 100} for n in range(1, capacity + 2))
        ok, jpeg = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
        self.assertTrue(ok)
        renderer = ComputeRenderer("windows")
        renderer.status["ready"] = True
        renderer.submit(replace(self.frame("UAV1", 0), image_jpeg=bytes(jpeg), tracks=tracks))
        draws = []
        with patch.object(cv2, "imshow"), patch.object(cv2, "waitKey", return_value=0), \
             patch.object(cv2, "imdecode", wraps=cv2.imdecode) as decode, \
             patch.object(cv2, "putText", side_effect=lambda image, label, *args: draws.append(label)), \
             patch.object(renderer, "_emit_frame_shown") as events, \
             patch("tracking_display.time.monotonic", side_effect=[0, METRICS_PAGE_SECONDS]):
            renderer.pump_once()
            self.assertEqual(sum(label == "legacy estimate / unqualified" for label in draws), capacity)
            self.assertFalse(any(label.startswith(f"L{capacity + 1}/") for label in draws))
            draws.clear()
            renderer.pump_once()
            self.assertEqual(sum(label == "legacy estimate / unqualified" for label in draws), 1)
            self.assertIn(f"L{capacity + 1}/G{capacity + 101}  ?km/h", draws)
            self.assertEqual(decode.call_count, 1)
            self.assertEqual(events.call_count, 1)
        self.assertEqual(renderer.status["shownFrames"], 1)

    def test_long_metric_label_is_trimmed_to_fit_with_ellipsis(self) -> None:
        label = "L123/G456  123.45km/h  270.0degT  H123.4/S234.5m  valid"
        measure_width = lambda value: len(value) * 8

        fitted = _fit_text(label, 80, measure_width)

        self.assertTrue(fitted.endswith("..."))
        self.assertLess(measure_width(fitted), measure_width(label))
        self.assertLessEqual(measure_width(fitted), 80)


if __name__ == "__main__":
    unittest.main()
