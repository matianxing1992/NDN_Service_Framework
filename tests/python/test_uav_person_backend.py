"""Backend contract checks; real CPU model evidence does not replace system qualification."""

from __future__ import annotations

import hashlib
import argparse
import copy
import json
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
from person_profile import parse_person_profile, prepare_person_profile  # noqa: E402
from person_reid import BackendFrame, PersonModelBackend  # noqa: E402

TEMPLATE = ROOT / "NDNSF-UAV-APP/configs/uav_person_tracking_demo.json"


class PersonBackendTest(unittest.TestCase):
    def test_unknown_detector_mode_rejected_before_model_loading(self):
        from dataclasses import replace
        profile = parse_person_profile(TEMPLATE.read_bytes())
        invalid = replace(profile, detector_preprocess_id="unknown-detector-mode")
        with mock.patch("person_reid.verify_person_model_files") as verify:
            with self.assertRaisesRegex(ValueError, "detector preprocessing"):
                PersonModelBackend(invalid, Path("unused.pt"), Path("unused.pth"))
            verify.assert_not_called()

    def test_unknown_reid_mode_rejected_before_model_loading(self):
        from dataclasses import replace
        profile = parse_person_profile(TEMPLATE.read_bytes())
        invalid = replace(profile, reid_preprocess_id="unknown-reid-mode")
        with mock.patch("person_reid.verify_person_model_files") as verify:
            with self.assertRaisesRegex(ValueError, "ReID preprocessing"):
                PersonModelBackend(invalid, Path("unused.pt"), Path("unused.pth"))
            verify.assert_not_called()

    def test_gem_feature_map_dispatch_formula_and_invalid_outputs(self):
        import torch

        from person_profile import GEM_REID_PREPROCESS_ID, GAP_REID_PREPROCESS_ID

        def make_backend(preprocess_id, feature_maps, pooled):
            calls = []

            class FakeReid(torch.nn.Module):
                def __init__(self):
                    super().__init__()
                    self.anchor = torch.nn.Parameter(torch.ones(1))

                def forward(self, batch, return_featuremaps=False):
                    calls.append(return_featuremaps)
                    output = feature_maps if return_featuremaps else pooled
                    return output.expand(batch.shape[0], *output.shape[1:])

            profile = SimpleNamespace(
                limits={"reidBatchSize": 1},
                reid_preprocess_id=preprocess_id,
            )
            backend = PersonModelBackend.__new__(PersonModelBackend)
            backend.profile = profile
            backend._torch = torch
            backend._device = torch.device("cpu")
            backend._reid = FakeReid()
            backend._transform = lambda _image: torch.zeros((3, 256, 128))
            return backend, calls

        image = np.zeros((64, 32, 3), dtype=np.uint8)
        boxes = [(0, 0, 32, 64)]
        uniform = torch.full((1, 512, 16, 8), 1.25)
        pooled_uniform = torch.full((1, 512), 1.25)
        gem, gem_calls = make_backend(GEM_REID_PREPROCESS_ID, uniform, pooled_uniform)
        gap, gap_calls = make_backend(GAP_REID_PREPROCESS_ID, uniform, pooled_uniform)
        uniform_features, device, batches = gem._features(image, boxes)
        gap_features, _, _ = gap._features(image, boxes)
        self.assertEqual((device, batches), ("cpu", 1))
        self.assertEqual(gem_calls, [True])
        self.assertEqual(gap_calls, [False])
        np.testing.assert_allclose(uniform_features, gap_features, rtol=0, atol=1e-7)

        sparse = torch.zeros((1, 512, 16, 8))
        sparse[0, 0, 0, 0] = 2.0
        sparse[0, 1, :, :] = 1.0
        sparse_backend, sparse_calls = make_backend(GEM_REID_PREPROCESS_ID, sparse,
                                                    pooled_uniform)
        first, _, _ = sparse_backend._features(image, boxes)
        second, _, _ = sparse_backend._features(image, boxes)
        self.assertEqual(sparse_calls, [True, True])
        self.assertEqual(first, second)
        actual = np.asarray(first[0], dtype=np.float64)
        channel_zero = ((2.0 ** 3 + 127 * (1e-6 ** 3)) / 128) ** (1 / 3)
        expected = np.full(512, 1e-6, dtype=np.float64)
        expected[0] = channel_zero
        expected[1] = 1.0
        expected /= np.linalg.norm(expected)
        np.testing.assert_allclose(actual, expected, rtol=1e-6, atol=1e-8)
        self.assertEqual(actual.shape, (512,))
        self.assertTrue(np.isfinite(actual).all())
        self.assertAlmostEqual(np.linalg.norm(actual), 1.0, places=6)

        invalid_maps = (
            (torch.ones((1, 512, 8, 16)), "shape"),
            (torch.full((1, 512, 16, 8), float("nan")), "finite"),
            (torch.full((1, 512, 16, 8), float("inf")), "finite"),
            (torch.zeros((1, 512, 16, 8)), "positive activation"),
        )
        for feature_maps, message in invalid_maps:
            backend, _ = make_backend(GEM_REID_PREPROCESS_ID, feature_maps, pooled_uniform)
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                backend._features(image, boxes)

    @unittest.skipUnless(os.environ.get("NDNSF_PERSON_MODEL_ROOT"),
                         "model bundle required for detector preprocessing fixture")
    def test_detector_preprocessing_geometry_shapes_stock_restore_and_cleanup(self):
        from ultralytics.nn.tasks import DetectionModel
        from ultralytics.trackers.basetrack import BaseTrack
        from byte_track_state import ByteTrackStateCodec

        self.addCleanup(setattr, BaseTrack, "_count", BaseTrack._count)
        root = Path(os.environ["NDNSF_PERSON_MODEL_ROOT"])
        detector = root / "yolo11n.pt"
        reid = Path(json.loads((root / "osnet-source.json").read_text())["path"])
        base = json.loads(prepare_person_profile(TEMPLATE, detector, reid).canonical_bytes)
        base["requestedDevice"] = "cpu"
        y, x = np.indices((640, 640))
        image = np.stack((x % 256, y % 256, (x + y) % 256), axis=-1).astype(np.uint8)
        digest = "sha256:" + hashlib.sha256(image.tobytes()).hexdigest()
        scripted = [[120, 180, 80, 180, .9]]
        shapes = []

        def detector_tensor(model, tensor, *args, **kwargs):
            shapes.append(tuple(tensor.shape))
            prediction = tensor.new_zeros((1, 84, len(scripted)))
            scale = tensor.shape[-1] / 640
            for index, row in enumerate(scripted):
                prediction[0, :4, index] = tensor.new_tensor(row[:4]) * scale
                prediction[0, 4, index] = row[4]
            return prediction

        def frames(sequence):
            return [BackendFrame({"cameraId": camera, "sourceId": "geometry/" + camera,
                                  "sourceEpoch": 1, "trackerGeneration": 1,
                                  "sequence": sequence, "commonPtsUs": (sequence - 1) * 500000,
                                  "inputDigest": digest}, image.copy())
                    for camera in ("UAV1", "UAV2", "UAV3")]

        states = []
        for mode in (None, "yolo-letterbox1280-raw-box-v1",
                     "yolo-letterbox1280-raw-box-nms50-v1"):
            explicit = mode is not None
            value = copy.deepcopy(base)
            if explicit:
                value.update(schema="uav-person-tracking-profile-v2",
                             detectorPreprocessId=mode)
            profile = parse_person_profile(json.dumps(value).encode())
            backend = PersonModelBackend(profile, detector, reid)
            try:
                with mock.patch.object(DetectionModel, "_predict_once", detector_tensor):
                    scripted = [[120, 180, 80, 180, .9]]
                    shapes.clear()
                    first = backend.process_window(frames(1))
                    predecessor = copy.deepcopy(first["backendState"])
                    scripted = [[140, 180, 80, 180, .9], [350, 180, 80, 180, .9],
                                [550, 180, 40, 100, .2]]
                    with mock.patch.object(backend, "_features", wraps=backend._features) as crops:
                        second = backend.process_window(frames(2), predecessor)
                    self.assertEqual(crops.call_count, 3)
                    self.assertTrue(shapes)
                    self.assertEqual(set(shapes), {(1, 3, 1280, 1280) if explicit else (1, 3, 640, 640)})
                    for c, frame in enumerate(second["frames"]):
                        rows = frame["detections"]
                        self.assertEqual([row["byteTrackId"] for row in rows], [1, 2, None])
                        self.assertIsNone(rows[2]["embedding"])
                        self.assertFalse(rows[2]["active"])
                        self.assertEqual(rows[2]["box"], [530, 130, 570, 230])
                        stock = second["backendState"]["cameras"][frame["cameraId"]]["tracker"]
                        self.assertFalse(stock["tracked"][1]["isActivated"])
                        stock_owners = backend._models[frame["cameraId"]].predictor.trackers[0].tracked_stracks
                        wanted = ([[100, 90, 180, 270], [310, 90, 390, 270]] if explicit else
                                  [[int(round(float(v))) for v in track.xyxy] for track in stock_owners])
                        self.assertEqual([row["box"] for row in rows[:2]], wanted)
                        self.assertEqual(crops.call_args_list[c].args[1], wanted)
                        if not explicit:
                            self.assertNotEqual(wanted[0], [100, 90, 180, 270])
                        for row in rows[:2]:
                            self.assertAlmostEqual(np.linalg.norm(row["embedding"]), 1, places=5)
                    replay = backend.process_window(frames(2), predecessor)
                    self.assertEqual(replay["frames"], second["frames"])
                    self.assertEqual(replay["backendState"], second["backendState"])
                    self.assertEqual(predecessor, first["backendState"])
                    fresh = PersonModelBackend(profile, detector, reid)
                    try:
                        resumed = fresh.process_window(frames(2), predecessor)
                        self.assertEqual(resumed["backendState"], second["backendState"])
                        for left, right in zip(resumed["frames"], second["frames"]):
                            for actual, wanted in zip(left["detections"], right["detections"]):
                                self.assertEqual({k: v for k, v in actual.items() if k != "embedding"},
                                                 {k: v for k, v in wanted.items() if k != "embedding"})
                                if wanted["embedding"] is not None:
                                    np.testing.assert_allclose(actual["embedding"], wanted["embedding"],
                                                               atol=1e-6, rtol=1e-6)
                                else:
                                    self.assertIsNone(actual["embedding"])
                    finally:
                        fresh.close()
                    self.assertEqual(fresh._hooks, [])
                    if explicit:
                        original = ByteTrackStateCodec._serialize_byte_tracker

                        def corrupt(tracker, **kwargs):
                            result = original(tracker, **kwargs)
                            tracker.tracked_stracks[0].idx = 1.25
                            return result

                        with mock.patch.object(ByteTrackStateCodec, "_serialize_byte_tracker", side_effect=corrupt), \
                                mock.patch.object(backend, "_features", wraps=backend._features) as features:
                            with self.assertRaisesRegex(ValueError, "stock detection index"):
                                backend.process_window(frames(2), predecessor)
                            features.assert_not_called()
                    states.append(second["backendState"])
                    # Exercise real stock NMS: these boxes have IoU .6, between the two modes.
                    scripted = [[120, 180, 80, 180, .9], [140, 180, 80, 180, .8]]
                    suppressed = backend.process_window(frames(1))
                    expected_count = 1 if mode == "yolo-letterbox1280-raw-box-nms50-v1" else 2
                    self.assertEqual([len(frame["detections"]) for frame in suppressed["frames"]],
                                     [expected_count] * 3)
            finally:
                backend.close()
            self.assertEqual(backend._hooks, [])
            self.assertEqual(backend._restore_callbacks, [])
        self.assertEqual(states[0], states[1])
        self.assertEqual(states[1], states[2])

    def test_session_protocol_reuses_factory_and_closes_on_eof_or_error(self):
        import tracking_worker as worker
        messages = [{"schema": "uav-person-worker-request-v1", "jobId": "sha256:" + "a" * 64,
                     "runId": "run", "missionId": "mission", "windowId": "window-" + str(i),
                     "sequence": i + 1, "computationDigest": "sha256:" + "b" * 64}
                    for i in range(2)]
        arguments = argparse.Namespace(run_id="run")
        raw = b"".join(json.dumps(m).encode() + b"\n" for m in messages)
        observed = []

        def compute(job, factory):
            engine = factory(SimpleNamespace(digest="profile"), Path("model"), Path("reid"))
            observed.append((job.window_id, job.sequence, job.expected_computation_digest, engine))
            return 0

        with mock.patch.object(worker.sys, "stdin", SimpleNamespace(buffer=io.BytesIO(raw))), \
                mock.patch.object(worker.sys, "stdout", io.StringIO()) as output, \
                mock.patch("person_reid.PersonModelBackend") as constructor, \
                mock.patch.object(worker, "_run_person_backend", side_effect=compute):
            self.assertEqual(worker._run_person_session(arguments), 0)
            self.assertEqual(constructor.call_count, 1)
            constructor.return_value.close.assert_called_once_with()
            self.assertEqual([x[:3] for x in observed],
                             [(m["windowId"], m["sequence"], m["computationDigest"]) for m in messages])
            self.assertIs(observed[0][3], observed[1][3])
            self.assertEqual(output.getvalue().count("SPEC219_PERSON_SESSION_DONE"), 2)

        def fail_after_load(job, factory):
            compute(job, factory)
            raise ValueError("invalid predecessor")

        with mock.patch.object(worker.sys, "stdin", SimpleNamespace(buffer=io.BytesIO(raw))), \
                mock.patch.object(worker.sys, "stdout", io.StringIO()) as output, \
                mock.patch("person_reid.PersonModelBackend") as constructor, \
                mock.patch.object(worker, "_run_person_backend", side_effect=fail_after_load):
            with self.assertRaisesRegex(ValueError, "invalid predecessor"):
                worker._run_person_session(arguments)
            constructor.return_value.close.assert_called_once_with()
            self.assertNotIn("SPEC219_PERSON_SESSION_DONE", output.getvalue())

    def test_session_invalid_control_never_constructs_models(self):
        import tracking_worker as worker
        valid = {"schema": "uav-person-worker-request-v1", "jobId": "sha256:" + "a" * 64,
                 "runId": "run", "missionId": "mission", "windowId": "window-0",
                 "sequence": 1, "computationDigest": "sha256:" + "b" * 64}
        invalid = []
        for key, value in [("sequence", True), ("runId", "other"), ("windowId", "window-1"),
                           ("jobId", "sha256:bad"), ("computationDigest", None), ("extra", 1)]:
            invalid.append(json.dumps({**valid, key: value}).encode() + b"\n")
        invalid.extend([json.dumps(valid).encode(), b"x" * 4097,
                        json.dumps(valid).encode()[:-1] + b',"sequence":1}\n'])
        for raw in invalid:
            with self.subTest(control=raw[:80]), \
                    mock.patch.object(worker.sys, "stdin", SimpleNamespace(buffer=io.BytesIO(raw))), \
                    mock.patch("person_reid.PersonModelBackend") as constructor, \
                    mock.patch.object(worker, "_run_person_backend") as compute:
                with self.assertRaises(ValueError):
                    worker._run_person_session(argparse.Namespace(run_id="run"))
                constructor.assert_not_called()
                compute.assert_not_called()

    def test_stock_crossing_ids_and_codec_resume(self):
        from byte_track_state import ByteTrackStateCodec
        from ultralytics.engine.results import Boxes
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker
        from ultralytics.utils import ROOT as ultralytics_root, YAML

        arguments = argparse.Namespace(**YAML.load(
            ultralytics_root / "cfg/trackers/bytetrack.yaml"))
        expected_ids = {"A": 1, "B": 2}

        def update(tracker, step):
            rows = [[40 + 4 * step, 50, 80 + 4 * step, 150, 0.9, 0],
                    [160 - 4 * step, 74, 200 - 4 * step, 174, 0.9, 0]]
            labels = ["A", "B"]
            if step % 2:
                rows.reverse()
                labels.reverse()
            boxes = Boxes(np.asarray(rows, dtype=np.float32), orig_shape=(220, 280))
            output = tracker.update(boxes)
            self.assertEqual(tracker.frame_id, step + 1)
            self.assertEqual(output.shape, (2, 8))
            observed = {}
            for row in output:
                index = int(row[-1])
                self.assertEqual(row[-1], index)
                self.assertIn(index, (0, 1))
                label = labels[index]
                self.assertNotIn(label, observed)
                self.assertEqual(row[4], int(row[4]))
                observed[label] = int(row[4])
            self.assertEqual(observed, expected_ids)
            return np.asarray(sorted(output, key=lambda row: row[4]))

        saved_counter = BaseTrack._count
        try:
            continuous = BYTETracker(arguments)
            reference = []
            checkpoint = None
            for step in range(31):
                reference.append(update(continuous, step))
                if step == 11:
                    checkpoint = ByteTrackStateCodec._serialize_byte_tracker(continuous)
            self.assertIsNotNone(checkpoint)
            resumed = BYTETracker(arguments)
            ByteTrackStateCodec._restore_byte_tracker_state(resumed, checkpoint)
            for step in range(12, 31):
                np.testing.assert_allclose(update(resumed, step), reference[step],
                                           atol=1e-4, rtol=0)
            state = ByteTrackStateCodec._serialize_byte_tracker(resumed)
            self.assertEqual(state["frameId"], 31)
            self.assertEqual(sorted(t["trackId"] for t in state["tracked"]), [1, 2])
            self.assertEqual(state["lost"], [])
            self.assertEqual(state["removed"], [])
            self.assertNotIn("globalId", json.dumps(state))
        finally:
            BaseTrack._count = saved_counter

    def test_stock_short_occlusion_recovers_same_id_after_codec_restore(self):
        from byte_track_state import ByteTrackStateCodec
        from ultralytics.engine.results import Boxes
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker
        from ultralytics.utils import ROOT as ultralytics_root, YAML

        arguments = argparse.Namespace(**YAML.load(
            ultralytics_root / "cfg/trackers/bytetrack.yaml"))

        def update(tracker, step):
            rows = [] if step in (2, 3) else [[10 + step, 20, 50 + step, 120, 0.9, 0]]
            boxes = Boxes(np.asarray(rows, dtype=np.float32).reshape(-1, 6),
                          orig_shape=(480, 640))
            output = tracker.update(boxes).copy()
            state = ByteTrackStateCodec._serialize_byte_tracker(tracker)
            self.assertEqual(state["frameId"], step + 1)
            self.assertEqual(state["removed"], [])
            if step in (2, 3):
                self.assertEqual(len(output), 0)
                self.assertEqual(state["tracked"], [])
                self.assertEqual([item["trackId"] for item in state["lost"]], [1])
                self.assertEqual([item["state"] for item in state["lost"]], [2])
            else:
                self.assertEqual(output.shape, (1, 8))
                self.assertEqual(output[0, 4], 1)
                self.assertEqual([item["trackId"] for item in state["tracked"]], [1])
                self.assertEqual(state["lost"], [])
            return output, state

        saved_counter = BaseTrack._count
        try:
            continuous = BYTETracker(arguments)
            self.assertGreater(continuous.max_frames_lost, 2)
            reference = []
            checkpoint = None
            for step in range(5):
                reference.append(update(continuous, step))
                if step == 2:
                    checkpoint = json.loads(json.dumps(reference[-1][1], allow_nan=False))
            before_restore = copy.deepcopy(checkpoint)
            resumed = BYTETracker(arguments)
            ByteTrackStateCodec._restore_byte_tracker_state(resumed, checkpoint)
            arrays = {"tlwh", "mean", "covariance"}
            for step in (3, 4):
                output, state = update(resumed, step)
                expected_output, expected_state = reference[step]
                np.testing.assert_allclose(output, expected_output, atol=1e-4, rtol=0)
                self.assertEqual(state["frameId"], expected_state["frameId"])
                for category in ("tracked", "lost", "removed"):
                    self.assertEqual(len(state[category]), len(expected_state[category]))
                    for actual, expected in zip(state[category], expected_state[category]):
                        self.assertEqual({k: v for k, v in actual.items() if k not in arrays},
                                         {k: v for k, v in expected.items() if k not in arrays})
                        for field in arrays:
                            np.testing.assert_allclose(actual[field], expected[field],
                                                       atol=1e-4, rtol=0)
            self.assertEqual(checkpoint, before_restore)
        finally:
            BaseTrack._count = saved_counter

    def test_stock_expiry_history_across_codec_restore(self):
        from byte_track_state import ByteTrackStateCodec
        from ultralytics.engine.results import Boxes
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker

        arguments = argparse.Namespace(track_buffer=1, track_high_thresh=0.25,
                                       track_low_thresh=0.1, new_track_thresh=0.25,
                                       match_thresh=0.8, fuse_score=True)
        detected = Boxes(np.asarray([[10, 20, 50, 120, 0.9, 0]], dtype=np.float32),
                         orig_shape=(480, 640))
        empty = Boxes(np.empty((0, 6), dtype=np.float32), orig_shape=(480, 640))
        saved_counter = BaseTrack._count
        try:
            continuous = BYTETracker(arguments)
            continuous.update(detected)
            continuous.update(empty)
            continuous.update(empty)
            self.assertEqual(continuous.frame_id, 3)
            self.assertEqual(len(continuous.lost_stracks), 1)
            self.assertEqual(len(continuous.removed_stracks), 1)
            self.assertIs(continuous.lost_stracks[0], continuous.removed_stracks[0])
            checkpoint = ByteTrackStateCodec._serialize_byte_tracker(continuous)
            # Continue the stock implementation independently before constructing a restored copy.
            reference = [continuous.update(boxes).copy() for boxes in (empty, detected, detected)]
            continuous_history = ByteTrackStateCodec._serialize_byte_tracker(continuous)
            self.assertEqual([t["trackId"] for t in continuous_history["removed"]], [1, 1])
            self.assertEqual(continuous_history["removed"][0], continuous_history["removed"][1])

            resumed = BYTETracker(arguments)
            ByteTrackStateCodec._restore_byte_tracker_state(resumed, checkpoint)
            self.assertIsNot(resumed.lost_stracks[0], resumed.removed_stracks[0])
            actual = [resumed.update(boxes).copy() for boxes in (empty, detected, detected)]
            for expected, observed in zip(reference, actual):
                np.testing.assert_allclose(observed, expected, atol=1e-6)
            history = ByteTrackStateCodec._serialize_byte_tracker(resumed)
            self.assertEqual([t["trackId"] for t in history["removed"]], [1, 1])
            self.assertEqual([t["state"] for t in history["removed"]], [3, 3])
            self.assertEqual([t["startFrame"] for t in history["removed"]], [1, 1])
            self.assertEqual([t["frameId"] for t in history["removed"]], [1, 1])
            self.assertNotEqual(history["removed"][0]["covariance"],
                                history["removed"][1]["covariance"])
            self.assertEqual(history["frameId"], 6)
            self.assertEqual([t["trackId"] for t in history["tracked"]], [2])
            self.assertEqual(len(history["lost"]), 0)
        finally:
            BaseTrack._count = saved_counter

    def test_compact_stock_history_preserves_long_fifo_and_live_restore(self):
        from byte_track_state import ByteTrackStateCodec
        from ultralytics.engine.results import Boxes
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker

        self.addCleanup(setattr, BaseTrack, "_count", BaseTrack._count)
        arguments = argparse.Namespace(track_buffer=1, track_high_thresh=.25,
                                       track_low_thresh=.1, new_track_thresh=.25,
                                       match_thresh=.8, fuse_score=True)
        continuous = BYTETracker(arguments)
        counter, predecessor = 0, None
        saw_duplicate_overlap = saw_truncation = False
        for step in range(1006 * 5):
            rows = [[100, 100, 140, 200, .9, 0]] if step % 5 < 2 else []
            boxes = Boxes(np.asarray(rows, dtype=np.float32).reshape(-1, 6), (480, 640))
            # Independent stock counter context; the reference never restores a snapshot.
            BaseTrack._count = counter
            expected = continuous.update(boxes).copy()
            counter = BaseTrack._count
            expected_history = [item.track_id for item in continuous.removed_stracks]
            resumed = BYTETracker(arguments)
            if predecessor is not None:
                ByteTrackStateCodec._restore_byte_tracker_state(
                    resumed, predecessor, compact_removed=True)
            actual = resumed.update(boxes).copy()
            state = ByteTrackStateCodec._serialize_byte_tracker(resumed, compact_removed=True)
            np.testing.assert_allclose(actual, expected, atol=1e-4, rtol=0)
            self.assertEqual(state["frameId"], continuous.frame_id)
            self.assertEqual(state["idHighWatermark"], counter)
            self.assertEqual(state["removedIds"], expected_history)
            self.assertLessEqual(len(state["removedIds"]), 1000)
            for group in ("tracked_stracks", "lost_stracks"):
                left, right = getattr(resumed, group), getattr(continuous, group)
                self.assertEqual([t.track_id for t in left], [t.track_id for t in right])
                for observed, reference in zip(left, right):
                    for field in ("state", "frame_id", "start_frame", "is_activated", "tracklet_len"):
                        self.assertEqual(getattr(observed, field), getattr(reference, field))
                    for field in ("mean", "covariance", "_tlwh"):
                        np.testing.assert_allclose(getattr(observed, field), getattr(reference, field),
                                                   atol=1e-4, rtol=0)
            saw_duplicate_overlap |= bool(set(expected_history) &
                                          {t.track_id for t in continuous.lost_stracks})
            saw_truncation |= len(expected_history) == 1000 and min(expected_history) > 1
            predecessor = state
        self.assertTrue(saw_duplicate_overlap)
        self.assertTrue(saw_truncation)
        self.assertEqual(counter, 1006)
        self.assertEqual(len(predecessor["removedIds"]), 1000)

    def test_compact_allocator_outlives_fifo_and_rejects_invalid_metadata(self):
        from byte_track_state import ByteTrackStateCodec, MAX_STOCK_TRACK_ID
        from ultralytics.engine.results import Boxes
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker

        self.addCleanup(setattr, BaseTrack, "_count", BaseTrack._count)
        arguments = argparse.Namespace(track_buffer=1, track_high_thresh=.25,
                                       track_low_thresh=.1, new_track_thresh=.25,
                                       match_thresh=.8, fuse_score=True)
        tracker = BYTETracker(arguments)
        rows = [[i * 10, 10, i * 10 + 8, 90, .9, 0] for i in range(512)]
        empty = Boxes(np.empty((0, 6), dtype=np.float32), (100, 6000))
        tracker.update(Boxes(np.asarray(rows, dtype=np.float32), (100, 6000)))
        for _ in range(3):
            tracker.update(Boxes(np.asarray(rows[:-1], dtype=np.float32), (100, 6000)))
        for _ in range(3):
            tracker.update(empty)
        self.assertEqual(BaseTrack._count, 512)
        self.assertEqual(len(tracker.removed_stracks), 1000)
        self.assertNotIn(512, [t.track_id for t in tracker.removed_stracks])
        state = ByteTrackStateCodec._serialize_byte_tracker(tracker, compact_removed=True)
        self.assertEqual(state["tracked"], [])
        self.assertEqual(state["lost"], [])
        self.assertEqual(state["idHighWatermark"], 512)
        resumed = BYTETracker(arguments)
        BaseTrack._count = 9999  # An unrelated camera must not influence declared restoration.
        ByteTrackStateCodec._restore_byte_tracker_state(resumed, state, compact_removed=True)
        self.assertEqual(BaseTrack._count, 512)
        original = copy.deepcopy(state)
        mutations = []
        for value in (True, -1, 1.5, MAX_STOCK_TRACK_ID, 1 << 64, 0):
            item = copy.deepcopy(state)
            item["idHighWatermark"] = value
            mutations.append(item)
        for value in ([0], [-1], [True], [1.5], [513], [1] * 1001, "not-a-list"):
            item = copy.deepcopy(state)
            item["removedIds"] = value
            mutations.append(item)
        item = copy.deepcopy(state)
        item["frameId"] = True
        mutations.append(item)
        item = copy.deepcopy(state)
        item["removed"] = []
        mutations.append(item)
        for invalid in mutations:
            with self.assertRaises(ValueError):
                ByteTrackStateCodec._restore_byte_tracker_state(resumed, invalid, compact_removed=True)
            self.assertEqual(BaseTrack._count, 512)
            self.assertEqual(ByteTrackStateCodec._serialize_byte_tracker(
                resumed, compact_removed=True), original)
        detected = Boxes(np.asarray(rows[:1], dtype=np.float32), (100, 6000))
        resumed.update(detected)
        self.assertEqual(resumed.tracked_stracks[0].track_id, 513)
        output = resumed.update(detected)
        self.assertEqual(int(output[0, 4]), 513)
        self.assertEqual(state, original)

    def test_production_compact_allocator_is_camera_local_and_replayable(self):
        from itertools import permutations
        from byte_track_state import ByteTrackStateCodec, MAX_STOCK_TRACK_ID
        from ultralytics.nn.tasks import DetectionModel
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker

        self.addCleanup(setattr, BaseTrack, "_count", BaseTrack._count)
        root = Path(os.environ["NDNSF_PERSON_MODEL_ROOT"])
        detector = root / "yolo11n.pt"
        reid = Path(json.loads((root / "osnet-source.json").read_text())["path"])
        value = json.loads(prepare_person_profile(TEMPLATE, detector, reid).canonical_bytes)
        value["requestedDevice"] = "cpu"
        profile = parse_person_profile(json.dumps(value).encode())
        image = np.zeros((640, 640, 3), dtype=np.uint8)
        digest = "sha256:" + hashlib.sha256(image.tobytes()).hexdigest()
        model_counts, extra = {}, 0

        def detector_tensor(model, tensor, *args, **kwargs):
            count = model_counts[id(model)] + extra
            prediction = tensor.new_zeros((1, 84, count))
            for i in range(count):
                prediction[0, :5, i] = tensor.new_tensor([80 + i * 150, 160, 40, 100, .9])
            return prediction

        def frames(sequence):
            return [BackendFrame({"cameraId": camera, "sourceId": "compact/" + camera,
                                  "sourceEpoch": 1, "trackerGeneration": 1,
                                  "sequence": sequence, "commonPtsUs": (sequence - 1) * 500000,
                                  "inputDigest": digest}, image.copy())
                    for camera in ("UAV1", "UAV2", "UAV3")]

        def register(backend):
            for index, model in enumerate(backend._models.values()):
                model_counts[id(model.model)] = index + 1

        backend = PersonModelBackend(profile, detector, reid)
        try:
            register(backend)
            with mock.patch.object(DetectionModel, "_predict_once", detector_tensor):
                first = backend.process_window(frames(1))
                self.assertEqual(first["backendState"]["schema"], "uav-person-backend-state-v3")
                predecessor = copy.deepcopy(first["backendState"])
                for index, camera in enumerate(("UAV1", "UAV2", "UAV3")):
                    self.assertEqual(predecessor["cameras"][camera]["tracker"]["idHighWatermark"], index + 1)
                # Restore existing trackers in every camera order, not only into fresh instances.
                arguments = argparse.Namespace(track_buffer=30, track_high_thresh=.25,
                                               track_low_thresh=.1, new_track_thresh=.25,
                                               match_thresh=.8, fuse_score=True)
                existing = BYTETracker(arguments)
                for order in permutations(("UAV1", "UAV2", "UAV3")):
                    for camera in order:
                        state = predecessor["cameras"][camera]["tracker"]
                        BaseTrack._count = 9999
                        ByteTrackStateCodec._restore_byte_tracker_state(
                            existing, state, compact_removed=True)
                        self.assertEqual(BaseTrack._count, state["idHighWatermark"])
                extra = 1
                second = backend.process_window(frames(2), predecessor)
                for index, camera in enumerate(("UAV1", "UAV2", "UAV3")):
                    self.assertEqual(second["backendState"]["cameras"][camera]["tracker"]["idHighWatermark"], index + 2)
                BaseTrack._count = 9999
                replay = backend.process_window(frames(2), predecessor)
                self.assertEqual(replay["backendState"], second["backendState"])
                self.assertEqual(replay["frames"], second["frames"])
                fresh = PersonModelBackend(profile, detector, reid)
                try:
                    register(fresh)
                    restored = fresh.process_window(frames(2), predecessor)
                    self.assertEqual(restored["backendState"], second["backendState"])
                    for left, right in zip(restored["frames"], second["frames"]):
                        self.assertEqual({k: v for k, v in left.items() if k != "detections"},
                                         {k: v for k, v in right.items() if k != "detections"})
                        self.assertEqual(len(left["detections"]), len(right["detections"]))
                        for actual, expected in zip(left["detections"], right["detections"]):
                            self.assertEqual({k: v for k, v in actual.items() if k != "embedding"},
                                             {k: v for k, v in expected.items() if k != "embedding"})
                            np.testing.assert_allclose(actual["embedding"], expected["embedding"],
                                                       atol=1e-6, rtol=1e-6)
                finally:
                    fresh.close()
                # One update can allocate beyond the remaining exact-ID range.
                exhausted = copy.deepcopy(predecessor)
                for state in exhausted["cameras"].values():
                    state["tracker"]["idHighWatermark"] = MAX_STOCK_TRACK_ID - 1
                extra = 2
                with self.assertRaisesRegex(ValueError, "exhausted"):
                    backend.process_window(frames(2), exhausted)
                self.assertEqual(BaseTrack._count, MAX_STOCK_TRACK_ID + 1)
                extra = 1
                repaired = backend.process_window(frames(2), predecessor)
                self.assertEqual(repaired["backendState"], second["backendState"])
                self.assertEqual(predecessor, first["backendState"])
        finally:
            backend.close()
        self.assertEqual(backend._hooks, [])
        self.assertEqual(backend._restore_callbacks, [])

    @unittest.skipUnless(os.environ.get("NDNSF_PERSON_MODEL_ROOT"),
                         "model bundle required for current stock-ID fixture")
    def test_current_stock_ids_before_confirmation_and_unowned_features(self):
        from ultralytics.engine.results import Boxes
        from ultralytics.nn.tasks import DetectionModel
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker
        from byte_track_state import ByteTrackStateCodec

        self.addCleanup(setattr, BaseTrack, "_count", BaseTrack._count)
        root = Path(os.environ["NDNSF_PERSON_MODEL_ROOT"])
        detector = root / "yolo11n.pt"
        reid = Path(json.loads((root / "osnet-source.json").read_text())["path"])
        value = json.loads(prepare_person_profile(TEMPLATE, detector, reid).canonical_bytes)
        value["requestedDevice"] = "cpu"
        profile = parse_person_profile(json.dumps(value).encode())
        image = np.zeros((640, 640, 3), dtype=np.uint8)
        digest = "sha256:" + hashlib.sha256(image.tobytes()).hexdigest()
        script = [
            [[100, 160, 40, 100, .9]],
            [[102, 160, 40, 100, .9], [300, 160, 40, 100, .9], [500, 160, 40, 100, .2]],
            [[104, 160, 40, 100, .9], [302, 160, 40, 100, .9], [500, 160, 40, 100, .2]],
            [[500, 160, 40, 100, .2]],
            [[500, 160, 40, 100, .9]],
            [[600, 160, 40, 100, .2]],
        ]
        arguments = SimpleNamespace(track_buffer=30, track_high_thresh=.25,
                                    track_low_thresh=.1, new_track_thresh=.25,
                                    match_thresh=.8, fuse_score=True)
        reference = BYTETracker(arguments)
        expected = []
        for rows in script:
            boxes = [[x - w / 2, y - h / 2, x + w / 2, y + h / 2, score, 0]
                     for x, y, w, h, score in rows]
            reference.update(Boxes(np.asarray(boxes, dtype=np.float32), image.shape[:2]))
            expected.append(ByteTrackStateCodec._serialize_byte_tracker(reference, compact_removed=True))
        self.assertFalse(expected[1]["tracked"][1]["isActivated"])
        self.assertEqual(expected[1]["tracked"][1]["trackId"], 2)
        self.assertTrue(expected[2]["tracked"][1]["isActivated"])
        self.assertEqual(expected[2]["tracked"][1]["trackId"], 2)

        def assert_stock_state(actual, wanted):
            # Restore preserves float32 Kalman arrays; discrete stock ownership/history is exact.
            actual = copy.deepcopy(actual)
            for group in ("tracked", "lost"):
                self.assertEqual(len(actual[group]), len(wanted[group]))
                for left, right in zip(actual[group], wanted[group]):
                    for field in ("mean", "covariance", "tlwh"):
                        np.testing.assert_allclose(left[field], right[field], atol=1e-4, rtol=0)
                        left[field] = right[field]
            self.assertEqual(actual, wanted)

        scripted = script[0]

        def detector_tensor(model, tensor, *args, **kwargs):
            prediction = tensor.new_zeros((1, 84, len(scripted)))
            for index, row in enumerate(scripted):
                prediction[0, :5, index] = tensor.new_tensor(row)
            return prediction

        def frames(sequence):
            return [BackendFrame({"cameraId": camera, "sourceId": "current/" + camera,
                                  "sourceEpoch": 1, "trackerGeneration": 1,
                                  "sequence": sequence, "commonPtsUs": (sequence - 1) * 500000,
                                  "inputDigest": digest}, image.copy())
                    for camera in ("UAV1", "UAV2", "UAV3")]

        backend = PersonModelBackend(profile, detector, reid)
        try:
            with mock.patch.object(DetectionModel, "_predict_once", detector_tensor):
                first = backend.process_window(frames(1))
                predecessor = copy.deepcopy(first["backendState"])
                self.assertEqual(predecessor["schema"], "uav-person-backend-state-v3")
                scripted = script[1]
                second = backend.process_window(frames(2), predecessor)
                for frame in second["frames"]:
                    self.assertEqual(len(frame["detections"]), 3)
                    self.assertEqual([d["byteTrackId"] for d in frame["detections"]], [1, 2, None])
                    for detection in frame["detections"][:2]:
                        self.assertTrue(detection["active"])
                        self.assertAlmostEqual(np.linalg.norm(detection["embedding"]), 1, places=5)
                    self.assertIsNone(frame["detections"][2]["embedding"])
                    self.assertFalse(frame["detections"][2]["active"])
                    assert_stock_state(second["backendState"]["cameras"][frame["cameraId"]]["tracker"], expected[1])
                replay = backend.process_window(frames(2), predecessor)
                self.assertEqual(replay["frames"], second["frames"])
                self.assertEqual(replay["backendState"], second["backendState"])
                fresh = PersonModelBackend(profile, detector, reid)
                try:
                    resumed = fresh.process_window(frames(2), predecessor)
                    self.assertEqual(resumed["backendState"], second["backendState"])
                    for left, right in zip(resumed["frames"], second["frames"]):
                        for actual, wanted in zip(left["detections"], right["detections"]):
                            self.assertEqual({k: v for k, v in actual.items() if k != "embedding"},
                                             {k: v for k, v in wanted.items() if k != "embedding"})
                            if wanted["embedding"] is None:
                                self.assertIsNone(actual["embedding"])
                            else:
                                np.testing.assert_allclose(actual["embedding"], wanted["embedding"],
                                                           atol=1e-6, rtol=1e-6)
                finally:
                    fresh.close()
                for version in ("uav-person-backend-state-v1", "uav-person-backend-state-v2"):
                    old = copy.deepcopy(predecessor)
                    old["schema"] = version
                    with self.assertRaisesRegex(ValueError, "schema"):
                        backend.process_window(frames(2), old)
                    self.assertEqual(predecessor, first["backendState"])
                original_serialize = ByteTrackStateCodec._serialize_byte_tracker
                invalid_owners = [("idx", value) for value in
                                  (True, 1.25, -1, 999, None, float("nan"), "duplicate")]
                invalid_owners += [("track_id", 0), ("track_id", True), ("track_id", 3),
                                   ("track_id", "duplicate"), ("cls", 1), ("cls", False),
                                   ("score", .8)]
                for field, invalid in invalid_owners:
                    def corrupt(tracker, **kwargs):
                        result = original_serialize(tracker, **kwargs)
                        if invalid == "duplicate":
                            setattr(tracker.tracked_stracks[1], field,
                                    getattr(tracker.tracked_stracks[0], field))
                        else:
                            setattr(tracker.tracked_stracks[0], field, invalid)
                        return result
                    with mock.patch.object(ByteTrackStateCodec, "_serialize_byte_tracker", side_effect=corrupt), \
                            mock.patch.object(backend, "_features", wraps=backend._features) as features:
                        with self.assertRaisesRegex(ValueError, "stock detection (index|owner)"):
                            backend.process_window(frames(2), predecessor)
                        features.assert_not_called()
                repaired = backend.process_window(frames(2), predecessor)
                self.assertEqual(repaired["backendState"], second["backendState"])
                scripted = script[2]
                third = backend.process_window(frames(3), second["backendState"])
                for frame in third["frames"]:
                    self.assertEqual([d["byteTrackId"] for d in frame["detections"]], [1, 2, None])
                    assert_stock_state(third["backendState"]["cameras"][frame["cameraId"]]["tracker"], expected[2])
                scripted = script[3]
                with mock.patch.object(backend._reid, "forward", wraps=backend._reid.forward) as forward:
                    fourth = backend.process_window(frames(4), third["backendState"])
                    forward.assert_not_called()
                self.assertEqual(fourth["reidInferenceBatches"], 0)
                self.assertEqual(fourth["actualReidDevice"], "not-run")
                for frame in fourth["frames"]:
                    self.assertEqual(len(frame["detections"]), 1)
                    self.assertIsNone(frame["detections"][0]["byteTrackId"])
                    self.assertIsNone(frame["detections"][0]["embedding"])
                    assert_stock_state(fourth["backendState"]["cameras"][frame["cameraId"]]["tracker"], expected[3])
                scripted = script[4]
                fifth = backend.process_window(frames(5), fourth["backendState"])
                for frame in fifth["frames"]:
                    self.assertEqual(frame["detections"][0]["byteTrackId"], 3)
                    tracker = fifth["backendState"]["cameras"][frame["cameraId"]]["tracker"]
                    self.assertFalse(tracker["tracked"][0]["isActivated"])
                    assert_stock_state(tracker, expected[4])
                scripted = script[5]
                with mock.patch.object(backend._reid, "forward", wraps=backend._reid.forward) as forward:
                    sixth = backend.process_window(frames(6), fifth["backendState"])
                    forward.assert_not_called()
                for frame in sixth["frames"]:
                    self.assertIsNone(frame["detections"][0]["byteTrackId"])
                    tracker = sixth["backendState"]["cameras"][frame["cameraId"]]["tracker"]
                    assert_stock_state(tracker, expected[5])
                    self.assertIn(3, tracker["removedIds"])
        finally:
            backend.close()
        self.assertEqual(backend._hooks, [])
        self.assertEqual(backend._restore_callbacks, [])

    def test_cli_admission_and_completion_diagnostic(self):
        from tracking_worker import _run_person_backend
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = json.loads(TEMPLATE.read_bytes())
            profile_path = root / "profile.json"
            profile_path.write_text(json.dumps(profile))
            context_path = root / "window.json"
            encoded = cv2.imencode(".jpg", np.zeros((32, 32, 3), np.uint8))[1].tobytes()
            digest = "sha256:" + hashlib.sha256(encoded).hexdigest()
            frames = [{"cameraId": camera, "sourceId": camera, "sourceEpoch": 1,
                       "trackerGeneration": 1, "sequence": 1, "commonPtsUs": 0,
                       "inputDigest": digest, "width": 32, "height": 32}
                      for camera in ("UAV1", "UAV2", "UAV3")]
            for frame in frames:
                (root / (frame["cameraId"] + ".jpg")).write_bytes(encoded)
            context = {"schema": "uav-person-backend-input-v1", "runId": "run",
                       "missionId": "mission", "windowId": "window", "sequence": 1,
                       "computationDigest": "sha256:" + "b" * 64, "frames": frames,
                       "previousBackendState": None}
            arguments = argparse.Namespace(
                tracking_profile=str(profile_path), person_window=str(context_path),
                reid_model="unused.pth", model="unused.pt", state_file=None,
                motion_json=None, observation_dir=None, observation_manifest=None,
                run_id="run", mission_id="mission", window_id="window", sequence=1,
                expected_computation_digest="sha256:" + "b" * 64,
                input_dir=str(root), output_json=str(root / "result.json"))
            cases = []
            mismatch = copy.deepcopy(context)
            mismatch["missionId"] = "another-mission"
            cases.append((json.dumps(mismatch), "does not match"))
            wrong_computation = copy.deepcopy(context)
            wrong_computation["computationDigest"] = "sha256:" + "c" * 64
            cases.append((json.dumps(wrong_computation), "computation binding mismatch"))
            cases.append((json.dumps(context)[:-1] + ',"runId":"run"}', "duplicate"))
            cases.append(("[]", "schema"))
            malformed = copy.deepcopy(context)
            malformed["frames"][0] = None
            cases.append((json.dumps(malformed), "canonical camera"))
            wrong_digest = copy.deepcopy(context)
            wrong_digest["frames"][0]["inputDigest"] = "sha256:" + "c" * 64
            cases.append((json.dumps(wrong_digest), "digest mismatch"))
            for dimension in ("width", "height"):
                missing = copy.deepcopy(context)
                del missing["frames"][0][dimension]
                cases.append((json.dumps(missing), "input frame fields"))
                for value in (True, "32", 0, -1):
                    invalid = copy.deepcopy(context)
                    invalid["frames"][0][dimension] = value
                    cases.append((json.dumps(invalid), "input dimensions"))
                mismatch = copy.deepcopy(context)
                mismatch["frames"][0][dimension] = 31
                cases.append((json.dumps(mismatch), "dimension limit"))
            with mock.patch("person_reid.PersonModelBackend") as constructor:
                for payload, message in cases:
                    with self.subTest(message=message):
                        context_path.write_text(payload)
                        with self.assertRaisesRegex(ValueError, message):
                            _run_person_backend(arguments)
                context_path.write_text(json.dumps(context))
                # Reject encoded size before decoding or model construction.
                (root / "UAV1.jpg").write_bytes(b"x" * (profile["limits"]["maxFrameBytes"] + 1))
                with self.assertRaisesRegex(ValueError, "byte limit"):
                    _run_person_backend(arguments)
                oversized = cv2.imencode(".jpg", np.zeros(
                    (32, profile["limits"]["maxFrameWidth"] + 1, 3), np.uint8))[1].tobytes()
                (root / "UAV1.jpg").write_bytes(oversized)
                context["frames"][0]["inputDigest"] = "sha256:" + hashlib.sha256(oversized).hexdigest()
                context_path.write_text(json.dumps(context))
                with self.assertRaisesRegex(ValueError, "dimension limit"):
                    _run_person_backend(arguments)
                constructor.assert_not_called()
                self.assertFalse(Path(arguments.output_json).exists())
                # Exercise the production completion path after successful admission.
                (root / "UAV1.jpg").write_bytes(encoded)
                context["frames"][0]["inputDigest"] = digest
                context_path.write_text(json.dumps(context))
                constructor.return_value.process_window.return_value = {}
                with mock.patch("builtins.print") as diagnostic:
                    self.assertEqual(_run_person_backend(arguments), 0)
                completed = json.loads(diagnostic.call_args.args[0])
                self.assertEqual(completed["event"], "PERSON_BACKEND_COMPLETED")
                self.assertEqual(completed["pid"], os.getpid())
                self.assertEqual(completed["windowId"], "window")
                self.assertLessEqual(completed["modelInitEndNs"], completed["inferenceEndNs"])
                sample = completed['modelResourceSample']
                self.assertEqual(sample['pid'], os.getpid())
                self.assertGreater(sample['creationTimeUnixSeconds'], 0)
                self.assertGreater(sample['rssBytes'], 0)
                self.assertGreater(sample['threads'], 0)
                self.assertLessEqual(completed['modelInitEndNs'], sample['monotonicNs'])
                self.assertLessEqual(sample['monotonicNs'], completed['inferenceStartNs'])
                constructor.return_value.close.assert_called_once_with()
                self.assertEqual(json.loads(Path(arguments.output_json).read_bytes())["runId"], "run")
                constructor.return_value.close.reset_mock()
                with mock.patch('tracking_worker._person_model_resource_sample',
                                side_effect=RuntimeError('unexpected observation failure')):
                    with self.assertRaisesRegex(RuntimeError, 'observation failure'):
                        _run_person_backend(arguments)
                constructor.return_value.close.assert_called_once_with()

    def test_model_resource_observation_reports_access_error(self):
        import psutil
        from tracking_worker import _person_model_resource_sample
        with mock.patch('psutil.Process', side_effect=psutil.AccessDenied(os.getpid())):
            self.assertEqual(_person_model_resource_sample(), {'error': 'AccessDenied'})

    def test_bad_context_rejected_before_backend_mutation(self):
        profile = parse_person_profile(TEMPLATE.read_bytes())
        backend = PersonModelBackend.__new__(PersonModelBackend)
        backend.profile = profile
        with self.assertRaisesRegex(ValueError, "three ordered"):
            backend.process_window([])
        frames = []
        for camera in ("UAV1", "UAV2", "UAV3"):
            metadata = {"cameraId": camera, "sourceId": camera, "sourceEpoch": True,
                        "trackerGeneration": 1, "sequence": 1, "commonPtsUs": 0,
                        "inputDigest": "sha256:" + "a" * 64}
            frames.append(BackendFrame(metadata, np.zeros((32, 32, 3), np.uint8)))
        with self.assertRaisesRegex(ValueError, "frame integer"):
            backend.process_window(frames)

    def test_missing_cuda_never_loads_models_or_falls_back(self):
        import torch
        if torch.cuda.is_available():
            self.skipTest("this negative case requires a host without CUDA")
        with tempfile.TemporaryDirectory() as directory:
            detector, reid = Path(directory) / "detector.pt", Path(directory) / "reid.pth"
            # Identity-only files deliberately cannot perform model inference.
            detector.write_bytes(b"not model weights")
            reid.write_bytes(b"not ReID weights")
            profile = prepare_person_profile(TEMPLATE, detector, reid)
            value = json.loads(profile.canonical_bytes)
            value["requestedDevice"] = "cuda:0"  # Explicit unavailable-device negative case.
            profile = parse_person_profile(json.dumps(value).encode())
            with self.assertRaisesRegex(RuntimeError, "CUDA_REQUIRED"):
                PersonModelBackend(profile, detector, reid)

    @unittest.skipUnless(os.environ.get("NDNSF_PERSON_MODEL_ROOT"),
                         "real model bundle not supplied; no model qualification claimed")
    def test_reused_models_restore_only_declared_state_and_bound_callbacks(self):
        root = Path(os.environ["NDNSF_PERSON_MODEL_ROOT"])
        detector = root / "yolo11n.pt"
        reid = Path(json.loads((root / "osnet-source.json").read_text())["path"])
        value = json.loads(prepare_person_profile(TEMPLATE, detector, reid).canonical_bytes)
        value["requestedDevice"] = "cpu"
        profile = parse_person_profile(json.dumps(value).encode())
        payload = (root / "bus.jpg").read_bytes()
        image = cv2.imdecode(np.frombuffer(payload, np.uint8), cv2.IMREAD_COLOR)

        def frames(sequence):
            return [BackendFrame({"cameraId": camera, "sourceId": "reuse/" + camera,
                                  "sourceEpoch": 1, "trackerGeneration": 1,
                                  "sequence": sequence, "commonPtsUs": (sequence - 1) * 500000,
                                  "inputDigest": "sha256:" + hashlib.sha256(payload).hexdigest()},
                                 image.copy()) for camera in ("UAV1", "UAV2", "UAV3")]

        def compare(actual, expected):
            self.assertEqual(actual["backendState"], expected["backendState"])
            for observed, reference in zip(actual["frames"], expected["frames"]):
                self.assertEqual(len(observed["detections"]), len(reference["detections"]))
                for left, right in zip(observed["detections"], reference["detections"]):
                    self.assertEqual({k: v for k, v in left.items() if k != "embedding"},
                                     {k: v for k, v in right.items() if k != "embedding"})
                    if right["embedding"] is None:
                        self.assertIsNone(left["embedding"])
                    else:
                        np.testing.assert_allclose(left["embedding"], right["embedding"],
                                                   atol=1e-6, rtol=1e-6)
                self.assertEqual(observed["lostByteTrackIds"], reference["lostByteTrackIds"])

        reused = PersonModelBackend(profile, detector, reid)
        try:
            models = dict(reused._models)
            reid_model = reused._reid
            first = reused.process_window(frames(1))
            state = copy.deepcopy(first["backendState"])
            initial_callbacks = {camera: len(model.callbacks["on_predict_start"])
                                 for camera, model in models.items()}
            second = reused.process_window(frames(2), state)
            predictors = {camera: model.predictor for camera, model in models.items()}
            replay = reused.process_window(frames(2), state)
            compare(replay, second)
            self.assertEqual(state, first["backendState"])
            self.assertEqual(len(reused._restore_callbacks), 3)
            for camera, model in models.items():
                self.assertIs(reused._models[camera], model)
                self.assertIsNot(model.predictor, predictors[camera])
                self.assertEqual(len(model.callbacks["on_predict_start"]),
                                 initial_callbacks[camera] + 1)
                self.assertEqual(replay["backendState"]["cameras"][camera]["tracker"]["frameId"], 2)
            self.assertIs(reused._reid, reid_model)
            compare(reused.process_window(frames(1)), first)
            self.assertEqual(reused._restore_callbacks, [])
            for camera, model in models.items():
                self.assertEqual(len(model.callbacks["on_predict_start"]), initial_callbacks[camera])

            fresh = PersonModelBackend(profile, detector, reid)
            try:
                compare(second, fresh.process_window(frames(2), state))
            finally:
                fresh.close()
        finally:
            reused.close()
            reused.close()
        self.assertEqual(reused._restore_callbacks, [])
        self.assertEqual(reused._hooks, [])
        self.assertEqual(reused._models, {})
        self.assertIsNone(reused._reid)

    @unittest.skipUnless(os.environ.get("NDNSF_PERSON_MODEL_ROOT"),
                         "model bundle required for production adapter fixture")
    def test_production_low_confidence_second_stage_resume_and_bounds(self):
        from ultralytics.engine.results import Boxes
        from ultralytics.nn.tasks import DetectionModel
        from ultralytics.trackers.basetrack import BaseTrack
        from ultralytics.trackers.byte_tracker import BYTETracker
        from ultralytics.utils import YAML
        from ultralytics.utils.checks import check_yaml
        from byte_track_state import ByteTrackStateCodec

        self.addCleanup(setattr, BaseTrack, "_count", BaseTrack._count)
        root = Path(os.environ["NDNSF_PERSON_MODEL_ROOT"])
        detector = root / "yolo11n.pt"
        reid = Path(json.loads((root / "osnet-source.json").read_text())["path"])
        value = json.loads(prepare_person_profile(TEMPLATE, detector, reid).canonical_bytes)
        value["requestedDevice"] = "cpu"
        image = np.zeros((640, 640, 3), dtype=np.uint8)
        digest = "sha256:" + hashlib.sha256(image.tobytes()).hexdigest()
        high = [[100, 160, 40, 100, .9]]
        low = [[102, 160, 40, 100, .2], [400, 160, 40, 100, .2],
               [500, 160, 40, 100, .05]]
        scripted = high

        def detector_tensor(model, tensor, *args, **kwargs):
            # Only the detector network output is controlled. Actual NMS, confidence
            # filtering, tracking callbacks and production state restoration still run.
            prediction = tensor.new_zeros((1, 84, max(1, len(scripted))))
            for index, row in enumerate(scripted):
                prediction[0, :5, index] = tensor.new_tensor(row)
            return prediction

        def frames(sequence):
            return [BackendFrame({"cameraId": camera, "sourceId": "low/" + camera,
                                  "sourceEpoch": 1, "trackerGeneration": 1,
                                  "sequence": sequence, "commonPtsUs": (sequence - 1) * 500000,
                                  "inputDigest": digest}, image.copy())
                    for camera in ("UAV1", "UAV2", "UAV3")]

        arguments = SimpleNamespace(**YAML.load(check_yaml("bytetrack.yaml")))
        continuous = BYTETracker(arguments)
        continuous.update(Boxes(np.asarray([[80, 110, 120, 210, .9, 0]],
                                          dtype=np.float32), image.shape[:2]))
        continuous_output = continuous.update(Boxes(
            np.asarray([[82, 110, 122, 210, .2, 0], [380, 110, 420, 210, .2, 0]],
                       dtype=np.float32), image.shape[:2]))
        self.assertEqual(len(continuous_output), 1,
                         "stock dependency must support low-confidence second-stage matching")
        self.assertEqual(int(continuous_output[0][4]), 1)
        expected_state = ByteTrackStateCodec._serialize_byte_tracker(continuous, compact_removed=True)
        backend = PersonModelBackend(parse_person_profile(json.dumps(value).encode()),
                                     detector, reid)
        try:
            with mock.patch.object(DetectionModel, "_predict_once", detector_tensor):
                first = backend.process_window(frames(1))
                predecessor = copy.deepcopy(first["backendState"])
                scripted = low
                second = backend.process_window(frames(2), predecessor)
                for before, after in zip(first["frames"], second["frames"]):
                    self.assertEqual(len(after["detections"]), 2)
                    matched, untracked = after["detections"]
                    self.assertEqual(matched["byteTrackId"], before["detections"][0]["byteTrackId"])
                    self.assertTrue(matched["active"])
                    self.assertAlmostEqual(matched["confidence"], .2, places=6)
                    self.assertIsNone(untracked["byteTrackId"])
                    self.assertFalse(untracked["active"])
                    self.assertEqual(untracked["box"], [380, 110, 420, 210])
                    self.assertEqual(after["lostByteTrackIds"], [])
                    state = copy.deepcopy(
                        second["backendState"]["cameras"][after["cameraId"]]["tracker"])
                    for group in ("tracked", "lost"):
                        self.assertEqual(len(state[group]), len(expected_state[group]))
                        for actual, expected in zip(state[group], expected_state[group]):
                            for field in ("mean", "covariance", "tlwh"):
                                np.testing.assert_allclose(actual[field], expected[field],
                                                           atol=1e-4, rtol=0)
                                actual[field] = expected[field]
                    self.assertEqual(state, expected_state)
                replay = backend.process_window(frames(2), predecessor)
                self.assertEqual(replay["backendState"], second["backendState"])
                self.assertEqual(replay["frames"], second["frames"])
                self.assertEqual(first["backendState"], predecessor)
                self.assertEqual(len(backend._restore_callbacks), 3)
        finally:
            backend.close()
        self.assertEqual(backend._hooks, [])
        self.assertEqual(backend._restore_callbacks, [])

        value["limits"]["maxPersonsPerFrame"] = 1
        bounded = PersonModelBackend(parse_person_profile(json.dumps(value).encode()), detector, reid)
        try:
            with mock.patch.object(DetectionModel, "_predict_once", detector_tensor):
                scripted = high
                first = bounded.process_window(frames(1))
                predecessor = copy.deepcopy(first["backendState"])
                scripted = low
                with self.assertRaisesRegex(ValueError, "PERSON_LIMIT"):
                    bounded.process_window(frames(2), predecessor)
                self.assertEqual(bounded._models["UAV1"].predictor.trackers[0].frame_id, 1)
                self.assertEqual(first["backendState"], predecessor)
                scripted = low[:1]
                repaired = bounded.process_window(frames(2), predecessor)
                self.assertEqual(len(repaired["frames"][0]["detections"]), 1)
                self.assertEqual(repaired["frames"][0]["detections"][0]["byteTrackId"],
                                 first["frames"][0]["detections"][0]["byteTrackId"])
        finally:
            bounded.close()
        self.assertEqual(bounded._hooks, [])
        self.assertEqual(bounded._restore_callbacks, [])

    @unittest.skipUnless(os.environ.get("NDNSF_PERSON_MODEL_ROOT"),
                         "real model bundle not supplied; no model qualification claimed")
    def test_real_cpu_features_and_single_update_tracker_restore(self):
        root = Path(os.environ["NDNSF_PERSON_MODEL_ROOT"])
        detector = root / "yolo11n.pt"
        manifest = json.loads((root / "osnet-source.json").read_text())
        reid = Path(manifest["path"])
        value = json.loads(prepare_person_profile(TEMPLATE, detector, reid).canonical_bytes)
        value["requestedDevice"] = "cpu"  # Explicit development run, never GPU qualification.
        profile = parse_person_profile(json.dumps(value).encode())
        payload = (root / "bus.jpg").read_bytes()
        image = cv2.imdecode(np.frombuffer(payload, np.uint8), cv2.IMREAD_COLOR)
        digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        def frames(sequence):
            return [BackendFrame({"cameraId": camera, "sourceId": "model-probe/" + camera,
                                  "sourceEpoch": 1, "trackerGeneration": 1,
                                  "sequence": sequence, "commonPtsUs": (sequence - 1) * 500000,
                                  "inputDigest": digest}, image.copy())
                    for camera in ("UAV1", "UAV2", "UAV3")]
        first = PersonModelBackend(profile, detector, reid)
        try:
            one = first.process_window(frames(1))
            self.assertEqual(one["actualDetectorDevice"], "cpu")
            self.assertEqual(one["detectorModelDevice"], "cpu")
            self.assertEqual(one["detectorInputDevice"], "cpu")
            self.assertEqual(one["actualReidDevice"], "cpu")
            self.assertEqual(one["reidInputDevice"], "cpu")
            self.assertEqual(one["reidModelDevice"], "cpu")
            self.assertGreater(one["reidInferenceBatches"], 0)
            self.assertNotIn("globalId", json.dumps(one))
            for frame in one["frames"]:
                self.assertTrue(frame["detections"])
                for detection in frame["detections"]:
                    self.assertEqual(detection["class"], "person")
                    feature = detection["embedding"]
                    if feature is not None:
                        self.assertEqual(len(feature), 512)
                        self.assertAlmostEqual(float(np.linalg.norm(feature)), 1.0, places=5)
            features, device, batches = first._features(image, [[50, 400, 245, 895]] * 2)
            self.assertEqual((device, batches), ("cpu", 1))
            np.testing.assert_allclose(features[0], features[1], atol=1e-5)
            invalid, device, batches = first._features(image, [[0, 0, 1, 1]])
            self.assertEqual((invalid, device, batches), ([None], "not-run", 0))
        finally:
            first.close()
        resumed = PersonModelBackend(profile, detector, reid)
        try:
            malformed = copy.deepcopy(one["backendState"])
            malformed["cameras"]["UAV1"]["sourceEpoch"] += 1
            with self.assertRaisesRegex(ValueError, "source/generation mismatch"):
                resumed.process_window(frames(2), malformed)
            two = resumed.process_window(frames(2), one["backendState"])
            for camera in ("UAV1", "UAV2", "UAV3"):
                # Exactly one stock update per input, including after restore.
                self.assertEqual(two["backendState"]["cameras"][camera]["tracker"]["frameId"], 2)
            for before, after in zip(one["frames"], two["frames"]):
                ids_before = {d["byteTrackId"] for d in before["detections"] if d["active"]}
                ids_after = {d["byteTrackId"] for d in after["detections"] if d["active"]}
                self.assertTrue(ids_before.intersection(ids_after))
        finally:
            resumed.close()
        value["limits"]["maxPersonsPerFrame"] = 1
        constrained = PersonModelBackend(parse_person_profile(json.dumps(value).encode()), detector, reid)
        try:
            with self.assertRaisesRegex(ValueError, "PERSON_LIMIT"):
                constrained.process_window(frames(1))
            # Raw detections are bounded before stock ByteTrack consumes the image.
            self.assertEqual(constrained._models["UAV1"].predictor.trackers[0].frame_id, 0)
        finally:
            constrained.close()


if __name__ == "__main__":
    unittest.main()
