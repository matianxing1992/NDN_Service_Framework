"""YOLO/ByteTrack/OSNet backend computation; native C++ owns all application IDs."""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import stat
import time
from typing import Any

import cv2
import numpy as np

from byte_track_state import ByteTrackStateCodec
from person_profile import (CAMERAS, GEM_REID_PREPROCESS_ID, GAP_REID_PREPROCESS_ID,
                            LEGACY_DETECTOR_PREPROCESS_ID, NMS50_DETECTOR_PREPROCESS_ID,
                            RAW_DETECTOR_PREPROCESS_ID, PersonProfile,
                            verify_person_model_files)


@dataclass(frozen=True)
class BackendFrame:
    metadata: dict[str, Any]
    image_bgr: np.ndarray


def _await_spec219_worker_cancel_gate(frame: "BackendFrame") -> None:
    """Hold one opt-in fault after real detector inference for MiniNDN cancellation."""
    raw_gate = os.environ.get("NDNSF_SPEC219_PERSON_WORKER_CANCEL_GATE")
    if raw_gate is None:
        return
    run_root_raw = os.environ.get("NDNSF_SPEC191_RUN_ROOT")
    if not run_root_raw:
        raise ValueError("person worker cancellation gate has no run root")
    run_root = Path(run_root_raw).resolve(strict=True)
    gate = Path(raw_gate)
    expected = run_root / "private/spec219-person-worker-cancel"
    if (not gate.is_absolute() or gate != expected or gate.is_symlink() or
            not gate.is_dir()):
        raise ValueError("person worker cancellation gate is outside its run root")
    gate_stat = gate.stat()
    if gate_stat.st_uid != os.geteuid() or stat.S_IMODE(gate_stat.st_mode) != 0o700:
        raise ValueError("person worker cancellation gate ownership or mode is invalid")
    metadata = frame.metadata
    sequence = metadata.get("sequence")
    camera = metadata.get("cameraId")
    if (type(sequence) is not int or sequence != 1 or camera != CAMERAS[0]):
        raise ValueError("person worker cancellation gate reached outside window zero")
    stat_text = Path("/proc/self/stat").read_text(encoding="ascii")
    fields = stat_text[stat_text.rfind(")") + 2:].split()
    if len(fields) <= 19:
        raise ValueError("person worker process identity is unavailable")
    marker = {
        "schema": "spec219-person-worker-active-v1",
        "runId": run_root.name,
        "windowId": "window-0",
        "sequence": sequence,
        "cameraId": camera,
        "phase": "after-first-detector-inference",
        "pid": os.getpid(),
        "startTimeTicks": int(fields[19]),
    }
    marker_path = gate / "detector-inference-active.json"
    temporary_path = gate / f".detector-inference-active-{os.getpid()}.tmp"
    payload = (json.dumps(marker, sort_keys=True, separators=(",", ":")) + "\n").encode()
    descriptor = os.open(temporary_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                         os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary_path, marker_path, follow_symlinks=False)
    finally:
        temporary_path.unlink(missing_ok=True)
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        release_path = gate / "release-inference"
        try:
            release_stat = release_path.lstat()
        except FileNotFoundError:
            release_stat = None
        if release_stat is not None:
            if (not stat.S_ISREG(release_stat.st_mode) or
                    release_stat.st_uid != os.geteuid()):
                raise ValueError("person worker gate release is not an owned regular file")
            return
        time.sleep(0.02)
    raise TimeoutError("person worker cancellation gate exceeded 30 seconds")


class PersonModelBackend:
    """Bounded, explicitly selected model execution with no global identity policy."""

    def __init__(self, profile: PersonProfile, detector_weights: Path, reid_weights: Path):
        if profile.detector_preprocess_id not in (LEGACY_DETECTOR_PREPROCESS_ID,
                                                  RAW_DETECTOR_PREPROCESS_ID,
                                                  NMS50_DETECTOR_PREPROCESS_ID):
            raise ValueError("unsupported person detector preprocessing")
        if profile.reid_preprocess_id not in (GAP_REID_PREPROCESS_ID, GEM_REID_PREPROCESS_ID):
            raise ValueError("unsupported person ReID preprocessing")
        verify_person_model_files(profile, detector_weights, reid_weights)
        import torch
        self.profile = profile
        self._torch = torch
        self._device = torch.device(profile.requested_device)
        if self._device.type == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA_REQUIRED: requested GPU is not available; no CPU fallback")
        from ultralytics import YOLO
        from ultralytics.trackers import register_tracker
        from torchreid.models import build_model
        from torchvision import transforms
        self._models = {}
        self._restored = set()
        self._restore_callbacks = []
        self._hooks = []
        self._detector_evidence = {}
        self._raw_detections = {}
        self._reid = None
        try:
            # All weights are existing digest-pinned files; pretrained=False prevents downloads.
            model = build_model("osnet_x1_0", num_classes=1, pretrained=False,
                                use_gpu=self._device.type == "cuda")
            checkpoint = torch.load(str(reid_weights), map_location="cpu", weights_only=True)
            state = checkpoint.get("state_dict", checkpoint)
            if not isinstance(state, dict):
                raise ValueError("OSNet checkpoint must contain a tensor state_dict")
            body = {}
            for name, tensor in state.items():
                name = name.removeprefix("module.")
                if not name.startswith("classifier."):
                    body[name] = tensor
            loaded = model.load_state_dict(body, strict=False)
            if loaded.unexpected_keys or set(loaded.missing_keys) != {
                    "classifier.weight", "classifier.bias"}:
                raise ValueError("OSNet checkpoint does not contain the complete requested model")
            self._reid = model.eval().to(self._device)
            self._transform = transforms.Compose([
                transforms.Resize((256, 128), interpolation=transforms.InterpolationMode.BILINEAR),
                transforms.ToTensor(),
                transforms.Normalize((0.485, 0.456, 0.406), (0.229, 0.224, 0.225)),
            ])
            for camera in CAMERAS:
                detector = YOLO(str(detector_weights), task="detect")
                self._models[camera] = detector
                maximum_people = profile.limits["maxPersonsPerFrame"]
                raw_cache = self._raw_detections
                def limit_detections(predictor, maximum_people=maximum_people,
                                     camera=camera, raw_cache=raw_cache):
                    if any(result.boxes is not None and len(result.boxes) > maximum_people
                           for result in predictor.results):
                        raise ValueError("PERSON_LIMIT")
                    if len(predictor.results) != 1:
                        raise ValueError("one source image per backend prediction is required")
                    raw = predictor.results[0].boxes
                    raw_cache[camera] = [] if raw is None else raw.data.detach().cpu().tolist()
                # This runs before stock tracking can discard unconfirmed detections.
                detector.add_callback("on_predict_postprocess_end", limit_detections)
                register_tracker(detector, persist=True)
                evidence = self._detector_evidence
                expected_device = str(self._device)
                def record_device(module, inputs, output, camera=camera,
                                  evidence=evidence, expected_device=expected_device):
                    def tensors(value):
                        if isinstance(value, torch.Tensor):
                            return [value]
                        if isinstance(value, (list, tuple)):
                            return [tensor for item in value for tensor in tensors(item)]
                        if isinstance(value, dict):
                            return [tensor for item in value.values() for tensor in tensors(item)]
                        return []
                    input_tensors, output_tensors = tensors(inputs), tensors(output)
                    parameters = list(module.parameters())
                    if (not input_tensors or not output_tensors
                            or not parameters
                            or any(str(t.device) != expected_device
                                   for t in input_tensors + output_tensors + parameters)):
                        raise RuntimeError("detector tensors executed on a different device")
                    evidence[camera] = expected_device
                self._hooks.append(detector.model.register_forward_hook(record_device))
            names = self._models[CAMERAS[0]].names
            person_classes = [int(key) for key, value in names.items() if value == "person"]
            if len(person_classes) != 1:
                raise ValueError("detector checkpoint must have exactly one person class")
            self._person_class = person_classes[0]
        except Exception:
            self.close()
            raise

    def close(self):
        self._reset_window_trackers()
        for hook in self._hooks:
            hook.remove()
        self._hooks.clear()
        self._raw_detections.clear()
        self._models.clear()
        self._restored.clear()
        self._reid = None

    def _reset_window_trackers(self):
        """Retain weights, but admit each window from its declared predecessor only."""
        for model, callback in self._restore_callbacks:
            model.callbacks["on_predict_start"].remove(callback)
        self._restore_callbacks.clear()
        self._restored.clear()
        self._raw_detections.clear()
        self._detector_evidence.clear()
        for model in self._models.values():
            model.predictor = None

    def _features(self, image, boxes):
        from PIL import Image
        output = [None] * len(boxes)
        valid = []
        for index, (x1, y1, x2, y2) in enumerate(boxes):
            if x2 - x1 < 16 or y2 - y1 < 32:
                continue
            rgb = cv2.cvtColor(image[y1:y2, x1:x2], cv2.COLOR_BGR2RGB)
            valid.append((index, self._transform(Image.fromarray(rgb))))
        actual_device = "not-run"
        batches = 0
        for offset in range(0, len(valid), self.profile.limits["reidBatchSize"]):
            items = valid[offset:offset + self.profile.limits["reidBatchSize"]]
            batch = self._torch.stack([tensor for _, tensor in items]).to(self._device)
            if (str(batch.device) != str(self._device)
                    or any(str(parameter.device) != str(self._device)
                           for parameter in self._reid.parameters())):
                raise RuntimeError("OSNet parameters/input are on a different device")
            with self._torch.inference_mode():
                if self.profile.reid_preprocess_id == GAP_REID_PREPROCESS_ID:
                    features = self._reid(batch)
                elif self.profile.reid_preprocess_id == GEM_REID_PREPROCESS_ID:
                    feature_maps = self._reid(batch, return_featuremaps=True)
                    expected_shape = (len(items), 512, 16, 8)
                    if (not isinstance(feature_maps, self._torch.Tensor)
                            or tuple(feature_maps.shape) != expected_shape):
                        raise ValueError("OSNet feature maps must have shape (batch,512,16,8)")
                    if not self._torch.isfinite(feature_maps).all():
                        raise ValueError("OSNet feature maps must be finite")
                    has_positive_activation = (feature_maps > 0).reshape(len(items), -1).any(dim=1)
                    if not self._torch.all(has_positive_activation):
                        raise ValueError("OSNet feature maps need positive activation per sample")
                    features = (self._torch.clamp(feature_maps, min=1e-6).pow(3)
                                .mean(dim=(2, 3)).pow(1 / 3))
                    if not self._torch.isfinite(features).all():
                        raise ValueError("OSNet GeM output must be finite")
                else:
                    raise ValueError("unsupported person ReID preprocessing")
            batches += 1
            actual_device = str(features.device)
            if features.ndim != 2 or features.shape != (len(items), 512):
                raise ValueError("OSNet output must have shape (batch,512)")
            if actual_device != str(self._device):
                raise RuntimeError("OSNet inference executed on a different device")
            for (index, _), feature in zip(items, features):
                norm = self._torch.linalg.vector_norm(feature)
                if self._torch.isfinite(feature).all() and self._torch.isfinite(norm) and norm > 1e-12:
                    output[index] = (feature / norm).detach().cpu().numpy().astype(np.float32).tolist()
        return output, actual_device, batches

    def _restore(self, frames, previous):
        if previous is None:
            return
        if set(previous) != {"schema", "cameras"} or previous["schema"] != "uav-person-backend-state-v3":
            raise ValueError("invalid model backend state schema")
        if set(previous["cameras"]) != set(CAMERAS):
            raise ValueError("incomplete model backend camera state")
        for frame in frames:
            camera = frame.metadata["cameraId"]
            record = previous["cameras"][camera]
            if set(record) != {"sourceId", "sourceEpoch", "trackerGeneration", "tracker"}:
                raise ValueError("invalid backend state fields")
            for key in ("sourceId", "sourceEpoch", "trackerGeneration"):
                if record[key] != frame.metadata[key]:
                    raise ValueError("backend tracker source/generation mismatch")
            tracker_state = record["tracker"]
            ByteTrackStateCodec._compact_high_watermark(tracker_state)
            if type(tracker_state["frameId"]) is not int or tracker_state["frameId"] < 0:
                raise ValueError("invalid backend tracker frame ID")
            groups = [tracker_state[key] for key in ("tracked", "lost")]
            if any(not isinstance(group, list) for group in groups):
                raise ValueError("backend track lists are required")
            count = sum(len(group) for group in groups)
            if count > self.profile.limits["maxBackendTracksPerCamera"]:
                raise ValueError("BACKEND_TRACK_LIMIT")
            for group in groups:
                for item in group:
                    for key in ("trackId", "state", "startFrame", "frameId",
                                "timeSinceUpdate", "trackletLength"):
                        if type(item[key]) is not int or item[key] < 0:
                            raise ValueError("invalid backend track counter")
                    if item["trackId"] == 0 or type(item["isActivated"]) is not bool:
                        raise ValueError("invalid backend track identity/state")
                    if not item["startFrame"] <= item["frameId"] <= tracker_state["frameId"]:
                        raise ValueError("backend track frame outside tracker history")
                    for key in ("score", "class", "index", "angle"):
                        scalar = item[key]
                        if key == "angle" and scalar is None:
                            continue
                        if type(scalar) not in (int, float) or not math.isfinite(scalar):
                            raise ValueError("invalid backend track scalar")
                    for key in ("tlwh", "mean", "covariance"):
                        array = np.asarray(item[key])
                        if array.dtype.kind not in "iuf" or not np.isfinite(array).all():
                            raise ValueError("invalid backend track array type/value")
            model = self._models[camera]
            restored = self._restored
            def restore_callback(predictor, persist=False, camera=camera, state=tracker_state,
                                 restored=restored):
                if camera in restored:
                    return
                if len(predictor.trackers) != 1:
                    raise ValueError("one isolated ByteTrack tracker per camera is required")
                ByteTrackStateCodec._restore_byte_tracker_state(
                    predictor.trackers[0], state, compact_removed=True)
                restored.add(camera)
            model.add_callback("on_predict_start", restore_callback)
            self._restore_callbacks.append((model, restore_callback))

    def process_window(self, frames: list[BackendFrame], previous_tracker_state=None):
        """Compute raw tracklets/features only; no global ID allocation or state commit."""
        if [frame.metadata.get("cameraId") for frame in frames] != list(CAMERAS):
            raise ValueError("exactly three ordered source frames are required")
        expected_keys = {"cameraId", "sourceId", "sourceEpoch", "trackerGeneration",
                         "sequence", "commonPtsUs", "inputDigest"}
        for frame in frames:
            metadata, image = frame.metadata, frame.image_bgr
            if set(metadata) != expected_keys or not isinstance(metadata["sourceId"], str):
                raise ValueError("invalid native backend frame context")
            for field in ("sourceEpoch", "trackerGeneration", "sequence", "commonPtsUs"):
                if type(metadata[field]) is not int or metadata[field] < (0 if field == "commonPtsUs" else 1):
                    raise ValueError("invalid native backend frame integer")
            if (not isinstance(image, np.ndarray) or image.dtype != np.uint8 or image.ndim != 3
                    or image.shape[2] != 3 or min(image.shape[:2]) < 1
                    or image.shape[0] > self.profile.limits["maxFrameHeight"]
                    or image.shape[1] > self.profile.limits["maxFrameWidth"]):
                raise ValueError("invalid or oversized native-staged image")
        self._reset_window_trackers()
        self._restore(frames, previous_tracker_state)
        start = time.monotonic_ns()
        output, states = [], {}
        detector_devices, reid_devices = set(), set()
        reid_batches = 0
        raw_geometry = self.profile.detector_preprocess_id in (
            RAW_DETECTOR_PREPROCESS_ID, NMS50_DETECTOR_PREPROCESS_ID)
        nms_iou = 0.5 if self.profile.detector_preprocess_id == NMS50_DETECTOR_PREPROCESS_ID else 0.7
        detector_options = {"imgsz": 1280, "rect": True, "iou": nms_iou} if raw_geometry else {}
        for frame_index, frame in enumerate(frames):
            camera, image = frame.metadata["cameraId"], frame.image_bgr
            model = self._models[camera]
            self._detector_evidence.pop(camera, None)
            # Predict with the stock tracking callbacks registered exactly once.
            # model.track() would register a second callback before a restored predictor exists.
            # Preserve stock ByteTrack's low-confidence second-stage association input.
            model.predict(image, mode="track", tracker="bytetrack.yaml",
                          classes=[self._person_class], conf=0.1,
                          max_det=self.profile.limits["maxPersonsPerFrame"] + 1,
                          device=str(self._device), verbose=False, **detector_options)
            if frame_index == 0:
                _await_spec219_worker_cancel_gate(frame)
            tracker = model.predictor.trackers[0]
            state = ByteTrackStateCodec._serialize_byte_tracker(tracker, compact_removed=True)
            if len(state["tracked"]) + len(state["lost"]) > self.profile.limits["maxBackendTracksPerCamera"]:
                raise ValueError("BACKEND_TRACK_LIMIT")
            detections = []
            boxes = []
            detector_device = self._detector_evidence.get(camera)
            if detector_device != str(self._device):
                raise RuntimeError("detector inference executed on a different device")
            raw = self._raw_detections.pop(camera, [])
            if len(raw) > self.profile.limits["maxPersonsPerFrame"]:
                raise ValueError("PERSON_LIMIT")
            for row in raw:
                if (len(row) != 6 or not all(type(value) in (int, float) and
                        math.isfinite(value) for value in row)
                        or row[5] != self._person_class or not 0 <= row[4] <= 1):
                    raise ValueError("invalid raw person detector output")
            owned = []
            consumed, local_ids = set(), set()
            for track in tracker.tracked_stracks:
                if track.state != 1 or track.frame_id != tracker.frame_id:
                    continue
                # Stock assigns IDs before confirmation. V3 exposes those current IDs
                # without changing stock confirmation, Kalman state or association.
                index = track.idx
                if (isinstance(index, (bool, np.bool_))
                        or not isinstance(index, (int, float, np.integer, np.floating))
                        or not math.isfinite(index) or int(index) != index
                        or not 0 <= index < len(raw) or int(index) in consumed):
                    raise ValueError("invalid current stock detection index")
                index = int(index)
                local_id = track.track_id
                if (isinstance(local_id, (bool, np.bool_))
                        or not isinstance(local_id, (int, np.integer))
                        or not 0 < local_id <= state["idHighWatermark"]
                        or local_id in local_ids
                        or any(isinstance(value, (bool, np.bool_))
                               or not isinstance(value, (int, float, np.integer, np.floating))
                               or not math.isfinite(value) for value in (track.cls, track.score))
                        or track.cls != raw[index][5] or track.score != raw[index][4]):
                    raise ValueError("invalid current stock detection owner")
                consumed.add(index)
                local_ids.add(int(local_id))
                # V2 crops the exact raw observation; stock still owns IDs and Kalman state.
                coordinates = raw[index][:4] if raw_geometry else track.xyxy
                owned.append((coordinates, float(track.score), int(local_id)))
            # Keep every raw detection. Only stock owners can use appearance features
            # in native global-ID admission; unowned observations remain UNTRACKED.
            selected = owned + [(row[:4], float(row[4]), None)
                                for index, row in enumerate(raw) if index not in consumed]
            for coordinates, score, local_id in selected:
                x1, y1, x2, y2 = [int(round(float(value))) for value in coordinates]
                h, w = image.shape[:2]
                rectangle = [max(0, min(w, x1)), max(0, min(h, y1)),
                             max(0, min(w, x2)), max(0, min(h, y2))]
                if rectangle[0] >= rectangle[2] or rectangle[1] >= rectangle[3]:
                    raise ValueError("invalid person detector box")
                detections.append({"class": "person", "byteTrackId": local_id,
                                   "box": rectangle, "confidence": score,
                                   "active": local_id is not None, "embedding": None})
                if local_id is not None:
                    boxes.append(rectangle)
            if len(detections) > self.profile.limits["maxPersonsPerFrame"]:
                raise ValueError("PERSON_LIMIT")
            features, reid_device, batches = self._features(image, boxes)
            reid_batches += batches
            for detection, feature in zip(detections, features):
                detection["embedding"] = feature
            detector_devices.add(detector_device)
            if batches:
                reid_devices.add(reid_device)
            tracker = model.predictor.trackers[0]
            states[camera] = {key: frame.metadata[key] for key in
                              ("sourceId", "sourceEpoch", "trackerGeneration")}
            states[camera]["tracker"] = state
            output.append({**frame.metadata, "detections": detections,
                           "lostByteTrackIds": [int(track.track_id) for track in tracker.lost_stracks]})
        if (detector_devices != {str(self._device)}
                or (reid_batches and reid_devices != {str(self._device)})):
            raise RuntimeError("inconsistent actual model devices")
        result = {"schema": "uav-person-backend-v1", "requestedDevice": str(self._device),
                  "actualDetectorDevice": str(self._device),
                  "detectorModelDevice": str(self._device),
                  "detectorInputDevice": str(self._device),
                  "actualReidDevice": str(self._device) if reid_batches else "not-run",
                  "reidInputDevice": str(self._device) if reid_batches else "not-run",
                  "reidModelDevice": str(next(self._reid.parameters()).device),
                  "reidInferenceBatches": reid_batches,
                  "detectorDigest": self.profile.detector_digest,
                  "reidDigest": self.profile.reid_digest, "profileDigest": self.profile.digest,
                  "frames": output, "backendState": {"schema": "uav-person-backend-state-v3",
                                                     "cameras": states},
                  "timing": {"modelWindowNs": time.monotonic_ns() - start}}
        if len(json.dumps(result, allow_nan=False).encode()) > self.profile.limits["maxResultBytes"]:
            raise ValueError("BACKEND_RESULT_LIMIT")
        return result
