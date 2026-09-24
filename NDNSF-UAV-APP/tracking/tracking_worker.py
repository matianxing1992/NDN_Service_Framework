#!/usr/bin/env python3
"""CPU tracking worker fed by verified frame bytes, never source paths.

The real profile uses the upstream Ultralytics YOLO checkpoint with its
ByteTrack tracker, one persistent tracker state per camera. The explicit
``functional`` profile is only for unit tests; it must never be reported as a
model qualification run.
"""

from __future__ import annotations

from dataclasses import dataclass
import argparse
import hashlib
import json
import math
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any, Iterable

import cv2
import numpy as np

CAMERAS = ("UAV1", "UAV2", "UAV3")


def _prefer_matching_mpl_toolkits() -> None:
    """Keep Matplotlib and mpl_toolkits from different distro layers apart.

    Ubuntu preloads ``mpl_toolkits`` from its system Matplotlib before the
    user-site Ultralytics dependency is imported.  The two versions expose
    incompatible ``Axes3D`` imports.  Put the user-site toolkit directory
    first when it is present; this changes import resolution only and does not
    alter model or tracking behavior.
    """
    module = sys.modules.get("mpl_toolkits")
    if module is None or not hasattr(module, "__path__"):
        return
    paths = list(module.__path__)
    for base in sys.path:
        candidate = Path(base) / "mpl_toolkits"
        if candidate.is_dir() and str(candidate) not in paths:
            paths.insert(0, str(candidate))
    module.__path__[:] = paths


@dataclass
class Track:
    local_id: int
    global_id: int
    label: str
    confidence: float
    box: tuple[int, int, int, int]
    pts_us: int

    def as_dict(self) -> dict[str, Any]:
        return {"localId": self.local_id, "globalId": self.global_id,
                "class": self.label, "confidence": round(self.confidence, 6),
                "box": list(self.box), "ptsUs": self.pts_us}


class CrossCameraAssociator:
    """Source-time, overlap-region association adapted from the upstream flow.

    The upstream project uses calibrated pixel polygons and wall-clock queues.
    This adapter keeps the same directional overlap idea but expresses regions
    as normalized coordinates and uses media PTS exclusively, which makes a
    replay at a different wall speed deterministic.
    """

    # The calibrated source and destination gates mirror the upstream
    # directional overlap rectangles.  The destination gate is deliberately
    # retained in the pending record: a source-side candidate must be observed
    # in the corresponding entry area before it can inherit an ID.  The old
    # implementation declared ``_ENTRY`` but never checked it, so any same
    # class detection in the destination camera could consume a candidate.
    _ROUTES = {
        "UAV1": {
            "destination": "UAV2",
            "sourceRect": (0.78, 0.32, 1.0, 0.87),
            "targetRect": (0.0, 0.32, 0.22, 0.81),
            "sourceLaneThreshold": 1355 / 2160,
            "targetLaneThreshold": 1225 / 2160,
        },
        "UAV2": {
            "destination": "UAV3",
            "sourceRect": (0.78, 0.43, 1.0, 0.92),
            "targetRect": (0.0, 0.21, 0.22, 0.69),
            "sourceLaneThreshold": 1455 / 2160,
            "targetLaneThreshold": 975 / 2160,
        },
    }

    def __init__(self, max_gap_us: int = 5_000_000, max_center_distance: float = 1.0) -> None:
        if max_gap_us <= 0 or not 0 < max_center_distance <= 1:
            raise ValueError("invalid cross-camera association bounds")
        self.max_gap_us = max_gap_us
        self.max_center_distance = max_center_distance
        self._next_global = 1
        self._mapping: dict[tuple[str, int], int] = {}
        self._pending: dict[str, list[dict[str, Any]]] = {camera: [] for camera in CAMERAS}

    @staticmethod
    def _center(box: tuple[int, int, int, int]) -> tuple[float, float]:
        return ((box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0)

    @staticmethod
    def _normalized(center: tuple[float, float], shape: tuple[int, int]) -> tuple[float, float]:
        width, height = shape
        return (center[0] / max(1, width), center[1] / max(1, height))

    @staticmethod
    def _in_rect(point: tuple[float, float], rect: tuple[float, float, float, float]) -> bool:
        return rect[0] <= point[0] <= rect[2] and rect[1] <= point[1] <= rect[3]

    @staticmethod
    def _lane(point: tuple[float, float], threshold: float = 0.57) -> str:
        return "upper" if point[1] < threshold else "lower"

    def observe(self, camera: str, local_id: int, label: str,
                box: tuple[int, int, int, int], pts_us: int,
                shape: tuple[int, int]) -> tuple[int, dict[str, Any] | None]:
        key = (camera, local_id)
        point = self._normalized(self._center(box), shape)
        existing = self._mapping.get(key)
        event: dict[str, Any] | None = None
        if existing is None:
            best: dict[str, Any] | None = None
            destination_candidates = self._pending.get(camera, [])
            for candidate in destination_candidates:
                if candidate["label"] != label:
                    continue
                gap = pts_us - candidate["ptsUs"]
                if gap < 0 or gap > self.max_gap_us:
                    continue
                if not self._in_rect(point, candidate["targetRect"]):
                    continue
                if candidate["lane"] != self._lane(point, candidate["targetLaneThreshold"]):
                    continue
                distance = ((point[0] - candidate["point"][0]) ** 2 +
                            (point[1] - candidate["point"][1]) ** 2) ** 0.5
                if distance > self.max_center_distance:
                    continue
                score = (1.0 - gap / self.max_gap_us) + (1.0 - distance)
                if best is None or score > best["score"]:
                    best = {"candidate": candidate, "score": score, "distance": distance}
            if best is not None:
                candidate = best["candidate"]
                existing = int(candidate["globalId"])
                event = {
                    "sourceCamera": candidate["sourceCamera"],
                    "sourceLocalId": candidate["sourceLocalId"],
                    "targetCamera": camera,
                    "targetLocalId": local_id,
                    "globalId": existing,
                    "sourcePtsUs": candidate["ptsUs"],
                    "targetPtsUs": pts_us,
                    "reason": "overlap-lane-pts-center",
                    "score": round(float(best["score"]), 6),
                }
                destination_candidates.remove(candidate)
            else:
                existing = self._next_global
                self._next_global += 1
            self._mapping[key] = existing
        global_id = existing
        route = self._ROUTES.get(camera)
        if route is not None and self._in_rect(point, route["sourceRect"]):
            destination = route["destination"]
            pending = {
                "sourceCamera": camera, "sourceLocalId": local_id,
                "globalId": global_id, "label": label, "point": point,
                "ptsUs": pts_us,
                "lane": self._lane(point, route["sourceLaneThreshold"]),
                "targetRect": route["targetRect"],
                "targetLaneThreshold": route["targetLaneThreshold"],
            }
            if not any(item["sourceLocalId"] == local_id for item in self._pending[destination]):
                self._pending[destination].append(pending)
        for destination in self._pending:
            self._pending[destination][:] = [item for item in self._pending[destination]
                                             if pts_us - item["ptsUs"] <= self.max_gap_us]
        return global_id, event

    def export_state(self) -> dict[str, Any]:
        """Return the bounded association state needed by the next worker process."""
        return {
            "maxGapUs": self.max_gap_us,
            "maxCenterDistance": self.max_center_distance,
            "nextGlobal": self._next_global,
            "mapping": [
                {"cameraId": camera, "localId": local_id, "globalId": global_id}
                for (camera, local_id), global_id in self._mapping.items()
            ],
            "pending": self._pending,
        }

    @classmethod
    def from_state(cls, state: dict[str, Any]) -> "CrossCameraAssociator":
        """Restore only validated, session-scoped association state."""
        associator = cls(int(state["maxGapUs"]), float(state["maxCenterDistance"]))
        associator._next_global = int(state["nextGlobal"])
        if associator._next_global < 1:
            raise ValueError("tracking association nextGlobal must be positive")
        for item in state["mapping"]:
            camera = str(item["cameraId"])
            local_id = int(item["localId"])
            global_id = int(item["globalId"])
            if camera not in CAMERAS or local_id < 1 or global_id < 1:
                raise ValueError("invalid tracking association mapping")
            associator._mapping[(camera, local_id)] = global_id
        pending = state["pending"]
        if set(pending) != set(CAMERAS):
            raise ValueError("tracking association pending cameras do not match")
        restored_pending: dict[str, list[dict[str, Any]]] = {camera: [] for camera in CAMERAS}
        for camera in CAMERAS:
            for item in pending[camera]:
                candidate = dict(item)
                source_camera = str(candidate.get("sourceCamera", ""))
                route = cls._ROUTES.get(source_camera)
                # Migrate snapshots written before destination gates were
                # persisted.  Such a snapshot is accepted only when its
                # source route is known; never invent a gate for arbitrary
                # state.
                if "targetRect" not in candidate or "targetLaneThreshold" not in candidate:
                    if route is None or route["destination"] != camera:
                        raise ValueError("tracking association pending route is not known")
                    candidate.setdefault("targetRect", route["targetRect"])
                    candidate.setdefault("targetLaneThreshold", route["targetLaneThreshold"])
                restored_pending[camera].append(candidate)
        associator._pending = restored_pending
        return associator


class TrackingEngine:
    def __init__(self, model_path: Path | None = None, *, functional: bool = False,
                 confidence: float = 0.25) -> None:
        self.functional = functional
        self.confidence = confidence
        self.model_path = model_path.resolve() if model_path else None
        self._models: dict[str, Any] = {}
        if not functional:
            if self.model_path is None or not self.model_path.is_file():
                raise FileNotFoundError("real tracking requires a local YOLO checkpoint")
        self._next_local = {camera: 1 for camera in ("UAV1", "UAV2", "UAV3")}
        self._next_global = 1
        self._last: dict[str, list[Track]] = {camera: [] for camera in self._next_local}
        self._associator = CrossCameraAssociator()
        self._last_associations: list[dict[str, Any]] = []
        # Ultralytics keeps ByteTrack/Kalman state inside each model
        # predictor.  The native node intentionally starts a bounded worker
        # per window, so this state must cross that process boundary too.
        self._byte_track_state: dict[str, dict[str, Any]] = {}
        self._byte_track_restored: set[str] = set()

    def start(self, profile: dict[str, Any], camera_profiles: list[dict[str, Any]]) -> None:
        """Freeze the CPU worker configuration before the first window."""
        configured_model = profile.get("modelPath")
        if configured_model is not None and Path(str(configured_model)).resolve() != self.model_path:
            raise ValueError("worker profile model does not match the pinned checkpoint")
        configured_cameras = [str(item.get("cameraId")) for item in camera_profiles]
        if tuple(configured_cameras) != CAMERAS:
            raise ValueError("worker camera order must be UAV1, UAV2, UAV3")
        if profile.get("device", "cpu") != "cpu":
            raise ValueError("Spec191 CPU worker accepts only device=cpu")

    def close(self) -> None:
        """Release model/tracker state after the bounded run."""
        self._models.clear()
        self._last = {camera: [] for camera in CAMERAS}
        self._associator = CrossCameraAssociator()
        self._byte_track_state = {}
        self._byte_track_restored = set()

    @staticmethod
    def _json_scalar(value: Any) -> Any:
        """Convert NumPy scalars to JSON values without accepting non-finite data."""
        if hasattr(value, "item"):
            value = value.item()
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("ByteTrack state contains a non-finite scalar")
        if isinstance(value, (str, int, float, bool)) or value is None:
            return value
        raise ValueError("ByteTrack state contains an unsupported scalar")

    @classmethod
    def _json_array(cls, value: Any, *, shape: tuple[int, ...] | None = None) -> list[Any] | None:
        if value is None:
            return None
        array = np.asarray(value)
        if shape is not None and tuple(array.shape) != shape:
            raise ValueError(f"ByteTrack state array shape {array.shape} != {shape}")
        if not np.isfinite(array).all():
            raise ValueError("ByteTrack state contains a non-finite array value")
        return array.astype(float).tolist()

    @classmethod
    def _serialize_byte_track(cls, track: Any) -> dict[str, Any]:
        """Serialize the state consumed by Ultralytics BYTETracker.update()."""
        tlwh = cls._json_array(getattr(track, "_tlwh", None), shape=(4,))
        mean = cls._json_array(getattr(track, "mean", None), shape=(8,))
        covariance = cls._json_array(getattr(track, "covariance", None), shape=(8, 8))
        if tlwh is None or mean is None or covariance is None:
            raise ValueError("ByteTrack state is missing an initialized Kalman track")
        return {
            "trackId": int(track.track_id),
            "isActivated": bool(track.is_activated),
            "state": int(track.state),
            "tlwh": tlwh,
            "mean": mean,
            "covariance": covariance,
            "score": float(cls._json_scalar(track.score)),
            "class": cls._json_scalar(track.cls),
            "index": cls._json_scalar(track.idx),
            "angle": cls._json_scalar(track.angle),
            "startFrame": int(track.start_frame),
            "frameId": int(track.frame_id),
            "timeSinceUpdate": int(track.time_since_update),
            "trackletLength": int(track.tracklet_len),
        }

    @classmethod
    def _restore_byte_track(cls, tracker: Any, item: dict[str, Any]) -> Any:
        """Reconstruct one validated Ultralytics STrack for a new process."""
        from ultralytics.trackers.byte_tracker import STrack

        tlwh = np.asarray(item["tlwh"], dtype=np.float32)
        mean = np.asarray(item["mean"], dtype=np.float32)
        covariance = np.asarray(item["covariance"], dtype=np.float32)
        if tlwh.shape != (4,) or mean.shape != (8,) or covariance.shape != (8, 8):
            raise ValueError("invalid ByteTrack state array shape")
        if not np.isfinite(tlwh).all() or not np.isfinite(mean).all() or not np.isfinite(covariance).all():
            raise ValueError("invalid non-finite ByteTrack state")
        if any(int(item[key]) < 0 for key in ("trackId", "startFrame", "frameId",
                                               "timeSinceUpdate", "trackletLength")):
            raise ValueError("invalid negative ByteTrack state counter")
        if int(item["trackId"]) == 0:
            raise ValueError("ByteTrack track IDs must be positive")
        x, y, width, height = (float(value) for value in tlwh)
        track = STrack(np.asarray([x + width / 2.0, y + height / 2.0, width, height,
                                   float(item["index"])], dtype=np.float32),
                       float(item["score"]), item["class"])
        track._tlwh = tlwh
        track.kalman_filter = tracker.kalman_filter
        track.mean = mean
        track.covariance = covariance
        track.track_id = int(item["trackId"])
        track.is_activated = bool(item["isActivated"])
        track.state = int(item["state"])
        track.score = float(item["score"])
        track.cls = item["class"]
        track.idx = item["index"]
        track.angle = item["angle"]
        track.start_frame = int(item["startFrame"])
        track.frame_id = int(item["frameId"])
        track.time_since_update = int(item["timeSinceUpdate"])
        track.tracklet_len = int(item["trackletLength"])
        track.history = OrderedDict()
        track.features = []
        track.curr_feature = None
        return track

    @classmethod
    def _serialize_byte_tracker(cls, tracker: Any) -> dict[str, Any]:
        return {
            "frameId": int(tracker.frame_id),
            "tracked": [cls._serialize_byte_track(item) for item in tracker.tracked_stracks],
            "lost": [cls._serialize_byte_track(item) for item in tracker.lost_stracks],
            "removed": [cls._serialize_byte_track(item) for item in tracker.removed_stracks],
        }

    @classmethod
    def _restore_byte_tracker_state(cls, tracker: Any, state: dict[str, Any]) -> None:
        from ultralytics.trackers.basetrack import BaseTrack

        frame_id = int(state["frameId"])
        if frame_id < 0:
            raise ValueError("ByteTrack frame ID must be non-negative")
        restored = {
            key: [cls._restore_byte_track(tracker, item) for item in state.get(key, [])]
            for key in ("tracked", "lost", "removed")
        }
        if any(len(items) > 5000 for items in restored.values()):
            raise ValueError("ByteTrack state exceeds bounded track count")
        tracker.frame_id = frame_id
        tracker.tracked_stracks = restored["tracked"]
        tracker.lost_stracks = restored["lost"]
        tracker.removed_stracks = restored["removed"]
        maximum_id = max((item.track_id for items in restored.values() for item in items), default=0)
        BaseTrack._count = max(int(BaseTrack._count), int(maximum_id))

    def _restore_tracker_callback(self, camera: str, predictor: Any) -> None:
        if camera in self._byte_track_restored:
            return
        state = self._byte_track_state.get(camera)
        if state is None:
            return
        trackers = getattr(predictor, "trackers", [])
        if len(trackers) != 1:
            raise RuntimeError("Spec191 expects exactly one ByteTrack tracker per camera")
        self._restore_byte_tracker_state(trackers[0], state)
        self._byte_track_restored.add(camera)

    def save_state(self, path: Path) -> None:
        """Persist the session tracker after a successfully processed window.

        The file is run-private and replaces the previous snapshot atomically;
        it is not a cache shared between missions or users.
        """
        state = {
            "schema": "spec191-tracking-session-state-v2",
            "cameras": list(CAMERAS),
            "nextLocal": dict(self._next_local),
            "last": {
                camera: [track.as_dict() for track in tracks]
                for camera, tracks in self._last.items()
            },
            "associator": self._associator.export_state(),
            "byteTrack": {
                camera: self._serialize_byte_tracker(self._models[camera].predictor.trackers[0])
                for camera in CAMERAS
                if camera in self._models and hasattr(self._models[camera].predictor, "trackers")
            },
        }
        path = path.resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(json.dumps(state, sort_keys=True) + "\n", encoding="utf-8")
        temporary.replace(path)

    def restore_state(self, path: Path) -> None:
        """Restore a run-private session snapshot before processing a window."""
        state = json.loads(path.read_text(encoding="utf-8"))
        if state.get("schema") not in {"spec191-tracking-session-state-v1",
                                        "spec191-tracking-session-state-v2"}:
            raise ValueError("unsupported tracking session state schema")
        if tuple(state.get("cameras", ())) != CAMERAS:
            raise ValueError("tracking session state camera order does not match")
        next_local = {camera: int(state["nextLocal"][camera]) for camera in CAMERAS}
        if any(value < 1 for value in next_local.values()):
            raise ValueError("tracking session local ids must be positive")
        last: dict[str, list[Track]] = {}
        for camera in CAMERAS:
            tracks: list[Track] = []
            for item in state["last"][camera]:
                box = tuple(int(value) for value in item["box"])
                if len(box) != 4 or int(item["localId"]) < 1 or int(item["globalId"]) < 1:
                    raise ValueError("invalid tracking session track")
                tracks.append(Track(int(item["localId"]), int(item["globalId"]),
                                    str(item["class"]), float(item["confidence"]),
                                    box, int(item["ptsUs"])))
            last[camera] = tracks
        self._next_local = next_local
        self._last = last
        self._associator = CrossCameraAssociator.from_state(state["associator"])
        byte_track = state.get("byteTrack", {})
        if set(byte_track) - set(CAMERAS):
            raise ValueError("tracking session ByteTrack cameras do not match")
        self._byte_track_state = {camera: dict(byte_track[camera]) for camera in byte_track}
        self._byte_track_restored = set()

    def process_window(self, window: dict[str, Any],
                       frames: Iterable[tuple[dict[str, Any], bytes]]) -> dict[str, Any]:
        """Process one deterministic three-camera window from verified bytes."""
        frame_list = list(frames)
        if len(frame_list) != len(CAMERAS):
            raise ValueError("one frame from each of UAV1, UAV2, UAV3 is required")
        outputs: list[dict[str, Any]] = []
        expected_sequence = int(window.get("sequence", 1))
        for expected_camera, (metadata, payload) in zip(CAMERAS, frame_list):
            camera = str(metadata.get("cameraId", ""))
            if camera != expected_camera:
                raise ValueError("window frames are not in canonical camera order")
            sequence = int(metadata.get("sequence", expected_sequence))
            pts_us = int(metadata.get("ptsUs", 0))
            if sequence != expected_sequence or pts_us < 0:
                raise ValueError("window metadata does not match the requested sequence")
            image = cv2.imdecode(np.frombuffer(payload, dtype=np.uint8), cv2.IMREAD_COLOR)
            if image is None:
                raise ValueError(f"invalid JPEG payload for {camera}")
            result = self.process_frame(camera, sequence, pts_us, image)
            result["inputDigest"] = "sha256:" + hashlib.sha256(payload).hexdigest()
            result["imageJpegDigest"] = "sha256:" + hashlib.sha256(result["imageJpeg"]).hexdigest()
            outputs.append(result)
        return {
            "schema": "spec191-tracking-result-v1",
            "runId": str(window.get("runId", "")),
            "missionId": str(window.get("missionId", "")),
            "windowId": str(window.get("windowId", "")),
            "sequence": expected_sequence,
            "algorithm": "yolo-bytetrack-cpu-v1" if not self.functional else "functional-test-v1",
            "frames": outputs,
            "associations": list(self._last_associations),
            "terminal": True,
        }

    @staticmethod
    def _centroid(box: tuple[int, int, int, int]) -> tuple[float, float]:
        return ((box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0)

    def _assign_ids(self, camera: str,
                    detections: list[tuple[tuple[int, int, int, int], float, str, int | None]],
                    pts_us: int) -> list[Track]:
        previous = self._last[camera]
        tracks: list[Track] = []
        used: set[int] = set()
        for box, confidence, label, tracker_id in detections:
            center = self._centroid(box)
            candidate = None
            best_distance = float("inf")
            for item in previous:
                if item.local_id in used or item.label != label:
                    continue
                distance = sum((a - b) ** 2 for a, b in zip(center, self._centroid(item.box)))
                if distance < best_distance and distance <= 100.0 ** 2:
                    candidate, best_distance = item, distance
            # Ultralytics tracker IDs are process-local.  After a window
            # boundary, prefer the restored session match and allocate a new
            # application-local ID for an unseen object instead of reusing a
            # fresh worker's ID.  The first window may retain ByteTrack's
            # positive ID as the initial local identity.
            if candidate is not None:
                local_id = candidate.local_id
            elif not previous and tracker_id is not None and tracker_id > 0:
                local_id = tracker_id
            else:
                local_id = self._next_local[camera]
                self._next_local[camera] += 1
            used.add(local_id)
            global_id, event = self._associator.observe(
                camera, local_id, label, box, pts_us, (self._frame_width, self._frame_height))
            if event is not None:
                self._last_associations.append(event)
            tracks.append(Track(local_id, global_id, label, confidence, box, pts_us))
        self._last[camera] = tracks
        return tracks

    def _detect(self, camera: str, image: np.ndarray
                ) -> list[tuple[tuple[int, int, int, int], float, str, int | None]]:
        height, width = image.shape[:2]
        if self.functional:
            return [((0, 0, width - 1, height - 1), 0.5, "car", None)]
        if camera not in self._models:
            _prefer_matching_mpl_toolkits()
            from ultralytics import YOLO
            from ultralytics.trackers import register_tracker
            assert self.model_path is not None
            model = YOLO(str(self.model_path))
            if camera in self._byte_track_state:
                # Install the stock initializer first, then restore our
                # validated state after it creates the BYTETracker.  The
                # model.track() call may register the stock callback again;
                # persist=True makes that second callback a no-op.
                register_tracker(model, persist=True)
                model.add_callback(
                    "on_predict_start",
                    lambda predictor, persist=False, camera=camera:
                    self._restore_tracker_callback(camera, predictor))
            self._models[camera] = model
        model = self._models[camera]
        detections: list[tuple[tuple[int, int, int, int], float, str, int | None]] = []
        # Ultralytics' built-in tracker is the pinned ByteTrack implementation.
        # A separate model instance per camera prevents tracker state from
        # leaking between independent source streams.
        results = model.track(image, persist=True, tracker="bytetrack.yaml",
                              conf=self.confidence, device="cpu", verbose=False)
        names = getattr(model, "names", {})
        for result in results:
            boxes = getattr(result, "boxes", None)
            if boxes is None:
                continue
            for box in boxes:
                score = float(box.conf[0])
                values = [int(round(float(value))) for value in box.xyxy[0].tolist()]
                label = str(names.get(int(box.cls[0]), "object"))
                tracker_id = None
                if getattr(box, "id", None) is not None:
                    tracker_id = int(box.id[0])
                detections.append(((values[0], values[1], values[2], values[3]),
                                   score, label, tracker_id))
        return detections

    def process_frame(self, camera: str, sequence: int, pts_us: int,
                      image_bgr: np.ndarray) -> dict[str, Any]:
        if camera not in self._last:
            raise ValueError(f"unknown camera: {camera}")
        if image_bgr is None or image_bgr.size == 0:
            raise ValueError("empty frame")
        self._frame_height, self._frame_width = image_bgr.shape[:2]
        self._last_associations = []
        tracks = self._assign_ids(camera, self._detect(camera, image_bgr), pts_us)
        annotated = image_bgr.copy()
        for track in tracks:
            x1, y1, x2, y2 = track.box
            cv2.rectangle(annotated, (x1, y1), (x2, y2), (0, 32, 255), 2)
            cv2.putText(annotated, f"{track.label} L{track.local_id} G{track.global_id}",
                        (x1, max(15, y1 - 4)), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 32, 255), 1)
        ok, encoded = cv2.imencode(".jpg", annotated, [cv2.IMWRITE_JPEG_QUALITY, 85])
        if not ok:
            raise RuntimeError("failed to encode annotated frame")
        return {"cameraId": camera, "sequence": sequence, "ptsUs": pts_us,
                "tracks": [track.as_dict() for track in tracks],
                "associations": list(self._last_associations),
                "imageJpeg": bytes(encoded),
                "inputDigest": "sha256:" + hashlib.sha256(image_bgr.tobytes()).hexdigest(),
                "algorithmProfile": "functional-test-v1" if self.functional else "yolo-bytetrack-cpu-v1"}


def _run_cli(arguments: argparse.Namespace) -> int:
    input_dir = Path(arguments.input_dir).resolve()
    model = Path(arguments.model).resolve()
    engine = TrackingEngine(model, confidence=arguments.confidence)
    engine.start({"device": "cpu", "modelPath": str(model)},
                 [{"cameraId": camera} for camera in CAMERAS])
    state_file = Path(arguments.state_file).resolve() if arguments.state_file else None
    if state_file is not None and state_file.is_file():
        engine.restore_state(state_file)
    frames: list[tuple[dict[str, Any], bytes]] = []
    for camera in CAMERAS:
        path = input_dir / f"{camera}.jpg"
        payload = path.read_bytes()
        frames.append(({"cameraId": camera, "sequence": arguments.sequence,
                        "ptsUs": arguments.pts_us}, payload))
    result = engine.process_window({"runId": arguments.run_id,
                                    "missionId": arguments.mission_id,
                                    "windowId": arguments.window_id,
                                    "sequence": arguments.sequence}, frames)
    annotated_dir = Path(arguments.annotated_dir).resolve()
    annotated_dir.mkdir(parents=True, exist_ok=True)
    for frame in result["frames"]:
        image = frame.pop("imageJpeg")
        image_path = annotated_dir / f"{frame['cameraId']}.jpg"
        image_path.write_bytes(image)
        frame["imagePath"] = str(image_path)
    if state_file is not None:
        engine.save_state(state_file)
    Path(arguments.output_json).write_text(json.dumps(result, sort_keys=True) + "\n",
                                           encoding="utf-8")
    engine.close()
    print(json.dumps({"event": "TRACKING_RESULT", "output": arguments.output_json,
                      "frames": len(result["frames"])}, sort_keys=True), flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--annotated-dir", required=True)
    parser.add_argument("--state-file")
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--mission-id", required=True)
    parser.add_argument("--window-id", required=True)
    parser.add_argument("--sequence", type=int, default=1)
    parser.add_argument("--pts-us", type=int, default=0)
    parser.add_argument("--confidence", type=float, default=0.25)
    args = parser.parse_args(argv)
    if args.sequence < 1 or args.pts_us < 0:
        parser.error("sequence must be positive and pts-us non-negative")
    try:
        return _run_cli(args)
    except Exception as error:
        print(json.dumps({"event": "TRACKING_FAILED", "error": str(error)}), flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
